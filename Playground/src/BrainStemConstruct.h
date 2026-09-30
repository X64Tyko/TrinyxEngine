#pragma once
#include "AnimConstruct.h" // AnimConstruct, AnimationAsset, SkeletonAsset, AssetRegistry, AssetTypes
#include "Construct.h"
#include "ConstructView.h"
#include "EPlayerCharacter.h"
#include "Logger.h"
#include "SimFloat.h"

// BrainStemConstruct — display-only skeletal character that loops its imported animation.
//
// Inherits AnimConstruct for per-instance bone cache and notify state.
// PostPhysics drives CAnimBase directly — no separate companion AnimBP Construct,
// so N instances produce N PostPhysics ticks but zero extra Construct registrations.
class BrainStemConstruct : public AnimConstruct, public Construct<BrainStemConstruct>
{
	TNX_REGISTER_CONSTRUCT(BrainStemConstruct)

public:
	TNX_CONSTRUCT_WORLD

	SimFloat SpawnPosX = SimFloat(0.f);
	SimFloat SpawnPosY = SimFloat(4.f);
	SimFloat SpawnPosZ = SimFloat(-4.f);

	ConstructView<EPlayerCharacter> Body;

	// Pre-warm asset loading before any BrainStemConstruct is spawned.
	// Checkouts in InitializeViews fire immediately on spawn if called first.
	static void PreloadAssets()
	{
		AssetRegistry& reg = AssetRegistry::Get();
		reg.TriggerLoad<AssetType::Mesh>(TNX_NAME("BrainStem"));
		reg.TriggerLoad<AssetType::Skeleton>(TNX_NAME("BrainStem"));
		reg.TriggerLoad<AssetType::Animation>(TNX_NAME("BrainStem_anim"));
	}

	void InitializeViews()
	{
		if (bIsClientSide)
		{
			Body.Attach(this, ReplicationEntityHandle);
			Body.SetFlags(TemporalFlagBits::Active | TemporalFlagBits::Alive | TemporalFlagBits::Replicated);
		}
		else
		{
			Body.Initialize(this);

			Vector3 spawnPos{ SpawnPosX, SpawnPosY, SpawnPosZ };
			Body.SetPosition(spawnPos);
			Body.Transform.Rotation.SetIdentity();

			// CAnimBase is Temporal+Net — replicated on the client path, so SetAnim stays server-only.
			Body.AnimBase.SetAnim(TNX_NAME("BrainStem_anim"), SimFloat(0.f), true);

			Body.SetFlags(TemporalFlagBits::Active | TemporalFlagBits::Alive | TemporalFlagBits::Replicated);
		}

		// CMeshRef (Cold) and CSkeletonRef (Volatile) are never replicated — must be set on both paths.
		Body.Mesh.SetMesh(TNX_NAME("BrainStem"));
		Body.SkeletonRef.SetSkeleton(TNX_NAME("BrainStem"));
		Body.VisTransform.VisBlend = SimFloat(1.f);

		if (auto skelRef = AssetRegistry::Get().GetAssetData<SkeletonAsset>(TNX_NAME("BrainStem")))
			RegisterSockets(skelRef);
		else
			LOG_WARN("[BrainStemConstruct] Skeleton not yet loaded — call PreloadAssets() before spawning");
	}

	void PostPhysics(SimFloat /*dt*/)
	{
		// Wide ESkeletalEntity::PostPhysics advances all timestamps each frame.
		// This scalar pass handles only state machine logic and loop wrapping.

		if (Body.Mesh.MeshID.Value() == 0) return; // wait for mesh slot to arrive

		const uint32_t baseAnimID = Body.AnimBase.BaseAnimID.Value();
		if (baseAnimID == 0) return;

		BeginAnimTick();

		// Base layer — wrap on loop
		const AssetDataRef<AnimationAsset> AssetRef = AssetRegistry::Get().GetAssetData<AnimationAsset>(AssetType::Animation, baseAnimID);
		const float baseDur                         = AssetRef->duration;
		SimFloat baseT                              = Body.AnimBase.GetBaseTimestamp();
		if (WrapTimestamp(baseT, baseDur, /*loops=*/true))
		{
			NotifyState.ClearLoopedRecords(0, SimFloat(baseDur));
			Body.AnimBase.SetBaseTimestamp(baseT);
		}

		// Fade layer — wrap source clip; clear when fully blended
		if (Body.AnimBase.HasFade())
		{
			const AssetDataRef<AnimationAsset> FadeRef = AssetRegistry::Get().GetAssetData<AnimationAsset>(AssetType::Animation, Body.AnimBase.FadeAnimID);
			const float fadeDur                        = FadeRef->duration;
			SimFloat fadeT                             = Body.AnimBase.GetFadeTimestamp();
			if (WrapTimestamp(fadeT, fadeDur, Body.AnimBase.GetFadeLoop()))
			{
				NotifyState.ClearLoopedRecords(1, SimFloat(fadeDur));
				Body.AnimBase.FadeTimestamp = fadeT;
			}
			if (Body.AnimBase.GetFadeAlpha() >= SimFloat(1.f)) Body.AnimBase.ClearFade();
		}

		// Overlay layers — wrap each active slot
		for (uint32_t s = 0; s < CAnimLayer<>::Slots; ++s)
		{
			const AssetDataRef<AnimationAsset> LayerRef = AssetRegistry::Get().GetAssetData<AnimationAsset>(AssetType::Animation, Body.AnimLayer.GetAnimID(s));
			if (!LayerRef) continue;
			const float layerDur = LayerRef->duration;
			SimFloat layerT      = Body.AnimLayer.GetTimestamp(s);
			if (WrapTimestamp(layerT, layerDur, CAnimLayer<>::GetLoop(Body.AnimLayer.GetConfig(s))))
			{
				NotifyState.ClearLoopedRecords(2 + s, SimFloat(layerDur));
				Body.AnimLayer.SetTimestamp(s, layerT);
			}
		}

		DebugAnimState(baseAnimID, baseT, baseDur);
	}

	void InitializeForReplication(WorldBase* world, EntityHandle* viewHandles, uint8_t viewCount)
	{
		bIsClientSide = true;
		if (viewCount > 0) ReplicationEntityHandle = viewHandles[0];
		Initialize(world);
	}

private:
	bool bIsClientSide = false;
	EntityHandle ReplicationEntityHandle{};

	static constexpr uint32_t kDebugLogInterval = 512; // 512Hz → ~1 log/sec

	uint32_t DebugTickCount = 0;
	bool SkeletonDumped     = false;

	void DebugAnimState(uint32_t animID, SimFloat t, float dur)
	{
		++DebugTickCount;

		if (!SkeletonDumped)
		{
			if (auto skelRef = AssetRegistry::Get().GetAssetData<SkeletonAsset>(TNX_NAME("BrainStem")))
			{
				LOG_INFO_F("[BrainStem] Skeleton boneCount=%u", skelRef->boneCount);
				for (uint32_t i = 0; i < skelRef->boneCount; ++i)
					LOG_INFO_F("[BrainStem]   bone[%2u] %-32s  parent=%u",
						i, skelRef->bones[i].name.GetStr(), skelRef->bones[i].parentIndex);
				SkeletonDumped = true;
			}
		}

		if (DebugTickCount % kDebugLogInterval != 0) return;

		const float tF = t.ToFloat();
		LOG_INFO_F("[BrainStem] animSlot=%u  t=%.3f / %.3f s  (%.1f%%)",
			animID, tF, dur, dur > 0.f ? 100.f * tF / dur : 0.f);

		static constexpr uint32_t kTrackedBone = 1;
		auto animRef                           = AssetRegistry::Get().GetAssetData<AnimationAsset>(AssetType::Animation, animID);
		if (!animRef) return;

		if (kTrackedBone < animRef->boneCount)
		{
			BoneTransform b = animRef->EvaluateBone(kTrackedBone, tF);
			LOG_INFO_F("[BrainStem]   bone[%u] tx=%.4f ty=%.4f tz=%.4f  rx=%.4f ry=%.4f rz=%.4f rw=%.4f",
				kTrackedBone,
				b.tx.ToFloat(), b.ty.ToFloat(), b.tz.ToFloat(),
				b.rx.ToFloat(), b.ry.ToFloat(), b.rz.ToFloat(), b.rw.ToFloat());
		}
		else
		{
			LOG_WARN_F("[BrainStem] kTrackedBone=%u out of range (boneCount=%u)", kTrackedBone, animRef->boneCount);
		}
	}
};
