#pragma once
#include "AnimationAsset.h"
#include "AnimTypes.h"
#include "AssetRegistry.h"
#include "AssetTypes.h"
#include "CAnimBase.h"
#include "CAnimLayer.h"
#include "CSkeletonRef.h"
#include "SimFloat.h"
#include "SkeletonAsset.h"

/// Mixin base for animation state machine Constructs.
/// PrePhysics applies root motion; PostPhysics runs the state machine and writes CAnimBase/CAnimLayer.
class AnimConstruct
{
public:
	/// Lazily evaluates the FK chain via BoneCache. Call RegisterSockets() first. PostPhysics only.
	BoneTransform GetSocketTransform(SocketID socket,
		const CAnimBase<FieldWidth::Scalar>& animBase,
		const CAnimLayer<FieldWidth::Scalar>& animLayer,
		uint32_t skeletonID)
	{
		SocketEntry* entry = SocketTransforms.FindSocket(socket);
		assert(entry && "Socket not registered — call RegisterSockets() after binding the skeleton");
		if (entry->valid) return entry->worldTransform;

		auto ref                  = AssetRegistry::Get().GetAssetData<SkeletonAsset>(AssetType::Skeleton, skeletonID);
		const SkeletonAsset* skel = ref.Get();
		if (!skel) return BoneTransform::Identity();

		ChainWalkResult walk = BoneCache.FindNearestCachedAncestor(entry->boneIndex, *skel);

		BoneTransform worldT = walk.ancestorTransform;
		for (uint32_t i = 0; i < walk.remainingBones; ++i)
		{
			uint32_t boneIdx     = walk.chain[i];
			BoneTransform localT = EvaluateBlendedBone(boneIdx, animBase, animLayer);
			worldT               = BoneTransform::Compose(worldT, localT);
			BoneCache.Insert(boneIdx, worldT);
		}

		entry->worldTransform = BoneTransform::Compose(worldT, entry->localOffset);
		entry->valid          = true;
		return entry->worldTransform;
	}

	/// Register sockets from a skeleton asset slot ID; no-op if the asset is not yet loaded.
	void RegisterSockets(uint32_t skeletonSlot)
	{
		RegisterSockets(AssetRegistry::Get().GetAssetData<SkeletonAsset>(AssetType::Skeleton, skeletonSlot));
	}

	/// Register sockets from a pre-resolved asset reference; populates SocketTransforms from SocketDef entries.
	void RegisterSockets(const AssetDataRef<SkeletonAsset>& skelRef)
	{
		const SkeletonAsset* skel = skelRef.Get();
		if (!skel) return;
		for (uint32_t i = 0; i < skel->socketCount; ++i)
		{
			const SocketDef& def = skel->sockets[i];
			SocketTransforms.RegisterSocket(def.id, def.boneIndex, def.localOffset);
		}
	}

protected:
	BoneCacheLocal BoneCache;
	SocketTransformLocal SocketTransforms;
	AnimNotifyState NotifyState;

	SimFloat RootMotionDeltaX{};
	SimFloat RootMotionDeltaY{};
	SimFloat RootMotionDeltaZ{};

	/// Clear bone and socket caches at the start of each PostPhysics tick.
	void BeginAnimTick()
	{
		BoneCache.Clear();
		SocketTransforms.Clear();
	}

	/// Sample the root motion delta for @p animID over [fromT, toT] and store it in RootMotionDelta{X,Y,Z}.
	void StoreRootMotionDelta(uint32_t animID, SimFloat fromT, SimFloat toT)
	{
		RootMotionDeltaX = SimFloat{};
		RootMotionDeltaY = SimFloat{};
		RootMotionDeltaZ = SimFloat{};

		auto ref                   = AssetRegistry::Get().GetAssetData<AnimationAsset>(AssetType::Animation, animID);
		const AnimationAsset* anim = ref.Get();
		if (!anim || !anim->hasRootMotion) return;

		BoneTransform delta = anim->EvaluateRootMotionDelta(fromT.ToFloat(), toT.ToFloat());
		RootMotionDeltaX    = delta.tx;
		RootMotionDeltaY    = delta.ty;
		RootMotionDeltaZ    = delta.tz;
	}

	/// Accumulate the stored root motion delta into the entity's world position fields.
	void ApplyRootMotion(FieldProxy<SimFloat, FieldWidth::Scalar>& posX,
		FieldProxy<SimFloat, FieldWidth::Scalar>& posY,
		FieldProxy<SimFloat, FieldWidth::Scalar>& posZ)
	{
		posX += RootMotionDeltaX;
		posY += RootMotionDeltaY;
		posZ += RootMotionDeltaZ;
	}

	/// Clamp or wrap @p inOutT to [0, duration] according to @p loops. Returns true if a loop occurred.
	static bool WrapTimestamp(SimFloat& inOutT, float duration, bool loops)
	{
		if (duration <= 0.f) return false;
		const SimFloat dur = SimFloat(duration);
		if (loops && inOutT >= dur)
		{
			inOutT = Fmod(inOutT, dur);
			return true;
		}
		if (!loops && inOutT > dur)
			inOutT = dur;
		return false;
	}

	/// Fire any notifies in @p animID whose trigger times fall within [fromT, toT], skipping duplicates.
	/// Calls `self->OnAnimNotify(NotifyFireEvent)` for each qualifying notify.
	template <typename Derived>
	void FireNotifies(Derived* self, uint32_t animID, uint32_t slot,
		SimFloat fromT, SimFloat toT, SimFloat weight)
	{
		auto ref                   = AssetRegistry::Get().GetAssetData<AnimationAsset>(AssetType::Animation, animID);
		const AnimationAsset* anim = ref.Get();
		if (!anim) return;
		for (const AnimNotifyDef& n : anim->notifies)
		{
			if (SimFloat(n.triggerTime) < fromT || SimFloat(n.triggerTime) >= toT) continue;
			if (NotifyState.HasFiredThisLoop(n.id, slot, fromT)) continue;
			self->OnAnimNotify({ n.id, weight, n.triggerTime });
			NotifyState.RecordFire(n.id, slot, SimFloat(n.triggerTime));
		}
	}

	/// Evaluate the fully blended local-space transform for a single bone on the CPU.
	/// Applies base/fade replace blend first, then additive overlay layers on top of the normalized result.
	static BoneTransform EvaluateBlendedBone(uint32_t boneIndex,
		const CAnimBase<FieldWidth::Scalar>& animBase,
		const CAnimLayer<FieldWidth::Scalar>& animLayer)
	{
		BoneTransform acc{};
		SimFloat totalW{};
		bool any = false;

		auto Accumulate = [&](uint32_t animID, SimFloat t, SimFloat w)
		{
			if (animID == 0 || w <= SimFloat(0)) return;
			auto ref                   = AssetRegistry::Get().GetAssetData<AnimationAsset>(AssetType::Animation, animID);
			const AnimationAsset* anim = ref.Get();
			if (!anim) return;
			BoneTransform sample = anim->EvaluateBone(boneIndex, t.ToFloat());
			if (!any)
			{
				acc = sample;
				acc.tx *= w;
				acc.ty *= w;
				acc.tz *= w;
				acc.rx *= w;
				acc.ry *= w;
				acc.rz *= w;
				acc.rw *= w;
				acc.sx *= w;
				acc.sy *= w;
				acc.sz *= w;
				totalW = w;
				any    = true;
			}
			else
			{
				acc    = BoneTransform::WeightedAdd(acc, sample, w);
				totalW = totalW + w;
			}
		};

		const SimFloat fadeAlpha = animBase.GetFadeAlpha();
		const SimFloat baseW     = SimFloat(1) - fadeAlpha;

		if (!animBase.IsBlendspaceMode())
			Accumulate(animBase.BaseAnimID.Value(), animBase.GetBaseTimestamp(), baseW);

		if (animBase.HasFade())
			Accumulate(animBase.FadeAnimID.Value(), animBase.GetFadeTimestamp(), fadeAlpha);

		for (uint32_t s = 0; s < CAnimLayer<FieldWidth::Scalar>::Slots; ++s)
		{
			const uint32_t id = animLayer.GetAnimID(s);
			if (id == 0) continue;
			const uint32_t cfg = animLayer.GetConfig(s);
			if (!CAnimLayer<FieldWidth::Scalar>::IsAdditive(cfg))
				Accumulate(id, animLayer.GetTimestamp(s), animLayer.GetAlpha(s));
		}

		if (!any) return BoneTransform::Identity();
		BoneTransform result = BoneTransform::Normalize(acc, totalW);

		// Additive layers are applied after the replace blend is normalized
		for (uint32_t s = 0; s < CAnimLayer<FieldWidth::Scalar>::Slots; ++s)
		{
			const uint32_t id = animLayer.GetAnimID(s);
			if (id == 0) continue;
			const uint32_t cfg = animLayer.GetConfig(s);
			if (!CAnimLayer<FieldWidth::Scalar>::IsAdditive(cfg)) continue;
			const SimFloat alpha = animLayer.GetAlpha(s);
			if (alpha <= SimFloat(0)) continue;
			auto ref                   = AssetRegistry::Get().GetAssetData<AnimationAsset>(AssetType::Animation, id);
			const AnimationAsset* anim = ref.Get();
			if (!anim) continue;
			BoneTransform addSample = anim->EvaluateBone(boneIndex, animLayer.GetTimestamp(s).ToFloat());
			result                  = BoneTransform::ApplyAdditive(result, addSample, alpha);
		}
		return result;
	}
};
