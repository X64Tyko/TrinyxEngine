#pragma once
#include <cstdint>
#include <cstring>
#include "TnxName.h"
#include "SimFloat.h"

/// Compact bone/socket identifier — FNV1a hash of the bone or socket name string.
using SocketID = uint32_t;
/// Compact animation notify identifier — FNV1a hash of the notify name string.
using NotifyID = uint32_t;

constexpr SocketID InvalidSocketID = 0;
constexpr NotifyID InvalidNotifyID = 0;

/// Produce a compile-time SocketID from a string literal.
#define TNX_SOCKET(str) (TnxName::Fnv1a(str))
/// Produce a compile-time NotifyID from a string literal.
#define TNX_NOTIFY(str) (TnxName::Fnv1a(str))

/// Value-type local or world-space bone transform evaluated by the animation system.
/// Translation/scale use SimFloat (Fixed32 metric precision).
/// Rotation uses SimUnit (FixedUnit precision) for bit-identical results under TNX_DETERMINISM.
struct BoneTransform
{
	SimFloat tx{}, ty{}, tz{};
	SimUnit rx{}, ry{}, rz{};
	SimUnit rw  = SimUnit(1);
	SimFloat sx = SimFloat(1), sy = SimFloat(1), sz = SimFloat(1);

	static BoneTransform Identity() { return {}; }

	/// Apply child transform in the local space of parent (full TRS composition).
	static BoneTransform Compose(const BoneTransform& parent, const BoneTransform& child);

	/// Normalized linear blend between two transforms by weight [0, 1].
	static BoneTransform NLerp(const BoneTransform& a, const BoneTransform& b, SimFloat t);

	/// Accumulate a weighted sample into an existing accumulator. Call Normalize() after all slots.
	static BoneTransform WeightedAdd(const BoneTransform& acc, const BoneTransform& sample, SimFloat w);
	/// Normalize an accumulated result produced by one or more WeightedAdd calls.
	static BoneTransform Normalize(const BoneTransform& acc, SimFloat totalWeight);

	/// Apply an additive delta authored from identity: translation adds, rotation composes base * nlerp(id, delta, alpha).
	static BoneTransform ApplyAdditive(const BoneTransform& base, const BoneTransform& delta, SimFloat alpha);
};

/// Single entry in a per-tick bone cache; stores a resolved world-space transform.
struct BoneCacheEntry
{
	uint32_t boneIndex = 0xFFFFFFFF;
	BoneTransform worldTransform;
};

/// Result of FindNearestCachedAncestor: the ancestor's world transform plus the
/// uncached chain that still needs FK evaluation (exclusive ancestor, inclusive target).
struct ChainWalkResult
{
	BoneTransform ancestorTransform; ///< Nearest cached ancestor (identity for root).
	uint32_t chain[64];              ///< Bone indices from ancestor toward target.
	uint32_t remainingBones = 0;
};

struct SkeletonAsset; // forward — resolved via SkeletonManager

/// PostPhysics-scoped bone cache; amortizes FK chain walks across multiple socket queries per tick.
/// Capacity is intentionally small — most Constructs query ≤16 unique bones per frame.
struct BoneCacheLocal
{
	static constexpr uint32_t MaxCachedBones = 16;

	BoneCacheEntry entries[MaxCachedBones];
	uint32_t count = 0;

	/// Reset the cache at the start of each PostPhysics tick.
	void Clear() { count = 0; }

	/// Returns the cached world transform for @p boneIndex, or nullptr if not yet evaluated.
	const BoneTransform* Find(uint32_t boneIndex) const
	{
		for (uint32_t i = 0; i < count; ++i)
			if (entries[i].boneIndex == boneIndex) return &entries[i].worldTransform;
		return nullptr;
	}

	/// Store a resolved world transform; silently drops the entry if the cache is full.
	void Insert(uint32_t boneIndex, const BoneTransform& t)
	{
		if (count < MaxCachedBones)
		{
			entries[count].boneIndex      = boneIndex;
			entries[count].worldTransform = t;
			++count;
		}
	}

	/// Walk up the skeleton hierarchy from @p targetBoneIndex to find the nearest cached ancestor.
	/// Returns the ancestor's world transform and the chain of bone indices
	/// (exclusive ancestor, inclusive target) that still need FK evaluation.
	ChainWalkResult FindNearestCachedAncestor(uint32_t targetBoneIndex,
		const SkeletonAsset& skeleton) const;
};

/// Per-socket evaluation state: resolved world transform plus validity flag.
struct SocketEntry
{
	SocketID id        = InvalidSocketID;
	uint32_t boneIndex = 0xFFFFFFFF;
	BoneTransform localOffset; ///< Offset in bone space from SocketDef.
	BoneTransform worldTransform;
	bool valid = false; ///< True once the world transform has been evaluated this tick.
};

/// PostPhysics-scoped socket cache; holds resolved world transforms for all registered sockets.
/// Validity flags are cleared at the start of each tick; entries are computed lazily on first query.
struct SocketTransformLocal
{
	static constexpr uint32_t MaxSockets = 12;

	SocketEntry sockets[MaxSockets];
	uint32_t count = 0;

	/// Invalidate all entries at the start of a PostPhysics tick without removing registrations.
	void Clear()
	{
		for (uint32_t i = 0; i < count; ++i)
			sockets[i].valid = false;
	}

	/// Register a socket so it can be queried via GetSocketTransform. Call once after skeleton bind.
	void RegisterSocket(SocketID id, uint32_t boneIndex, const BoneTransform& localOffset)
	{
		if (count >= MaxSockets) return;
		sockets[count].id          = id;
		sockets[count].boneIndex   = boneIndex;
		sockets[count].localOffset = localOffset;
		sockets[count].valid       = false;
		++count;
	}

	/// Returns the SocketEntry for @p id, or nullptr if the socket was not registered.
	SocketEntry* FindSocket(SocketID id)
	{
		for (uint32_t i = 0; i < count; ++i)
			if (sockets[i].id == id) return &sockets[i];
		return nullptr;
	}
};

/// Payload delivered to AnimConstruct::OnAnimNotify when a notify crosses its trigger time.
struct NotifyFireEvent
{
	NotifyID id;
	SimFloat blendWeight; ///< Weight of the blend slot that fired this notify.
	float triggerTime;
};

/// Record of a single notify fire; used by AnimNotifyState to prevent double-fire within a loop.
struct NotifyFireRecord
{
	NotifyID id;
	uint32_t animSlot;
	SimFloat firedAtTime;
};

/// Per-tick deduplication state for animation notifies; prevents double-fire within a loop iteration.
/// Not persistent across ticks — Clear() is called by AnimConstruct::BeginAnimTick each frame.
struct AnimNotifyState
{
	static constexpr uint32_t MaxFireRecords = 32;

	NotifyFireRecord fired[MaxFireRecords];
	uint32_t firedCount = 0;

	/// Reset all fire records at the start of a tick.
	void Clear() { firedCount = 0; }

	/// Returns true if @p id has already been fired for @p slot at or after @p loopStart this tick.
	bool HasFiredThisLoop(NotifyID id, uint32_t slot, SimFloat loopStart) const
	{
		for (uint32_t i = 0; i < firedCount; ++i)
			if (fired[i].id == id && fired[i].animSlot == slot && fired[i].firedAtTime >= loopStart)
				return true;
		return false;
	}

	/// Record that @p id was fired for @p slot at @p time; silently drops if the buffer is full.
	void RecordFire(NotifyID id, uint32_t slot, SimFloat time)
	{
		if (firedCount >= MaxFireRecords) return;
		fired[firedCount++] = { id, slot, time };
	}

	/// Remove all records for @p slot whose fire time predates @p newLoopStart (loop wrap occurred).
	void ClearLoopedRecords(uint32_t slot, SimFloat newLoopStart)
	{
		uint32_t write = 0;
		for (uint32_t i = 0; i < firedCount; ++i)
			if (!(fired[i].animSlot == slot && fired[i].firedAtTime < newLoopStart))
				fired[write++] = fired[i];
		firedCount = write;
	}
};
