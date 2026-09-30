#include "TestFramework.h"

#include "AnimationAsset.h"
#include "AnimTypes.h"
#include "SkeletonAsset.h"

#include <cmath>
#include <cstring>

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static constexpr float kEps = 1e-4f;

// Under TNX_DETERMINISM each RotateVectorFixed truncates to the nearest Fixed32 unit
// (0.1mm = 1e-4m). The truncation error also leaks between axes: a Z-vector rotated 90°
// around Y produces ox=9999 and oz=1 instead of (10000, 0). Each rotated Compose adds
// one LSB of error per affected axis; this 5-bone chain has 2 such calls.
#ifdef TNX_DETERMINISM
static constexpr float kEpsPos = 3e-4f;
#else
static constexpr float kEpsPos = kEps;
#endif

static bool NearEq(float a, float b)
{
	return std::abs(a - b) < kEps;
}
static bool NearEq(float a, float b, float eps)
{
	return std::abs(a - b) < eps;
}
template <typename T>
static bool NearEq(SimFloatImpl<T> a, float b)
{
	return NearEq(a.ToFloat(), b);
}
template <typename T>
static bool NearEq(SimFloatImpl<T> a, float b, float eps)
{
	return NearEq(a.ToFloat(), b, eps);
}

// 5-bone straight chain: bone i has parent i-1 (bone 0 is root with 0xFFFFFFFF).
// inverseBindPose is identity (only needed by GPU skinning, not CPU socket eval).
static SkeletonAsset MakeChainSkeleton()
{
	SkeletonAsset skel;
	skel.boneCount = 5;
	skel.bones.resize(5);
	for (uint32_t i = 0; i < 5; ++i)
	{
		BoneInfo& b   = skel.bones[i];
		b.parentIndex = (i == 0) ? 0xFFFFFFFF : (i - 1);
		std::memset(b.inverseBindPose, 0, sizeof(b.inverseBindPose));
		b.inverseBindPose[0]  = 1.f;
		b.inverseBindPose[5]  = 1.f;
		b.inverseBindPose[10] = 1.f;
		b.inverseBindPose[15] = 1.f;
	}
	return skel;
}

// 5-bone animation where each bone has local offset tz=1 from its parent.
// At t=0: all identity rotations.
// At t=1: bone 2 gains a 90° rotation around Y.
static AnimationAsset MakeChainAnimation()
{
	const float s45 = std::sqrt(2.f) * 0.5f; // sin(45°) = cos(45°) for 90° half-angle

	AnimationAsset anim;
	anim.duration  = 1.0f;
	anim.boneCount = 5;
	anim.boneTracks.resize(5);
	anim.keyframes.reserve(10);

	for (uint32_t b = 0; b < 5; ++b)
	{
		anim.boneTracks[b].keyframeOffset = b * 2;
		anim.boneTracks[b].keyframeCount  = 2;

		AnimKeyframe k0{}; // t=0: local z+1 from parent, identity rotation
		k0.time = 0.f;
		k0.tz   = 1.f;
		k0.rw   = 1.f;

		AnimKeyframe k1 = k0; // t=1: same position, bone 2 rotated 90° around Y
		k1.time         = 1.f;
		if (b == 2)
		{
			k1.ry = s45;
			k1.rw = s45;
		}

		anim.keyframes.push_back(k0);
		anim.keyframes.push_back(k1);
	}
	return anim;
}

// Compose the world transform for targetBone by walking the chain from root.
// Uses a simple straight-chain assumption (bone i's parent is i-1).
static BoneTransform EvalChainWorld(const AnimationAsset& anim, uint32_t targetBone, float t)
{
	// Root joint is always at origin — bone 0's local translation is not applied to its own
	// position, only its rotation propagates down. Start with bone 0's rotation but zero pos.
	BoneTransform local0 = anim.EvaluateBone(0, t);
	local0.tx = local0.ty = local0.tz = 0.f;
	BoneTransform world               = local0;
	for (uint32_t b = 1; b <= targetBone; ++b)
		world = BoneTransform::Compose(world, anim.EvaluateBone(b, t));
	return world;
}

// -----------------------------------------------------------------------
// Test 1 — EvaluateBone correctness + known socket world position
//
// At t=1, bone 2 is rotated 90° around Y.  Bones 3 and 4 follow with a 1-unit
// local-Z offset each — which after the rotation becomes a world-X step.
//
// Expected world positions:
//   bone 0: (0, 0, 0)     bone 1: (0, 0, 1)    bone 2: (0, 0, 2)
//   bone 3: (1, 0, 2)     bone 4: (2, 0, 2)
// -----------------------------------------------------------------------

TEST(Anim_Correctness_KnownAnimation)
{
	AnimationAsset anim = MakeChainAnimation();
	const float s45     = std::sqrt(2.f) * 0.5f;

	// Bone 2 local at t=1 must have 90° rotation around Y and tz=1
	BoneTransform bone2 = anim.EvaluateBone(2, 1.0f);
	ASSERT(NearEq(bone2.rx, 0.f));
	ASSERT(NearEq(bone2.ry, s45));
	ASSERT(NearEq(bone2.rz, 0.f));
	ASSERT(NearEq(bone2.rw, s45));
	ASSERT(NearEq(bone2.tz, 1.f));

	// Bone 0 must remain identity rotation at t=1
	BoneTransform bone0 = anim.EvaluateBone(0, 1.0f);
	ASSERT(NearEq(bone0.rx, 0.f));
	ASSERT(NearEq(bone0.rw, 1.f));

	// Straight-chain sanity at t=0: bone 4 world pos should be (0, 0, 4)
	BoneTransform world4_t0 = EvalChainWorld(anim, 4, 0.0f);
	ASSERT(NearEq(world4_t0.tx, 0.f));
	ASSERT(NearEq(world4_t0.ty, 0.f));
	ASSERT(NearEq(world4_t0.tz, 4.f));

	// At t=1, bone 2's 90° rotation bends bones 3+4 along world X
	BoneTransform world4_t1 = EvalChainWorld(anim, 4, 1.0f);
	ASSERT(NearEq(world4_t1.tx, 2.f, kEpsPos)); // 2 units along world X after the bend
	ASSERT(NearEq(world4_t1.ty, 0.f));
	ASSERT(NearEq(world4_t1.tz, 2.f, kEpsPos)); // bones 0+1+2 each contribute 1 unit along Z

	// Rotation at bone 4 should still be 90° around Y (no additional rotation from bones 3/4)
	ASSERT(NearEq(world4_t1.ry, s45));
	ASSERT(NearEq(world4_t1.rw, s45));

	// Midpoint at t=0.5: bone 2 rotation nlerp'd to ~45° (ry < s45, rw > s45)
	BoneTransform bone2_half = anim.EvaluateBone(2, 0.5f);
	ASSERT(bone2_half.ry > 0.f && bone2_half.ry < s45);  // growing toward 90°
	ASSERT(bone2_half.rw > s45 && bone2_half.rw <= 1.f); // shrinking toward cos(45°)
}

// -----------------------------------------------------------------------
// Test 2 — BoneCache warm ancestor reuse
//
// With bones 0/1/2 cached, querying bone 4 should find bone 2 as the
// nearest ancestor and return chain=[3,4].  After inserting bone 3,
// querying bone 4 again should return chain=[4] (only one bone to eval).
// -----------------------------------------------------------------------

TEST(Anim_BoneCache_AncestorWalk)
{
	SkeletonAsset skel = MakeChainSkeleton();

	BoneCacheLocal cache;
	cache.Clear();

	const BoneTransform id = BoneTransform::Identity();

	// Seed cache with bones 0, 1, 2
	cache.Insert(0, id);
	cache.Insert(1, id);
	cache.Insert(2, id);
	ASSERT_EQ(cache.count, 3u);

	// Query bone 3: nearest cached ancestor is bone 2 → chain=[3]
	{
		ChainWalkResult r = cache.FindNearestCachedAncestor(3, skel);
		ASSERT_EQ(r.remainingBones, 1u);
		ASSERT_EQ(r.chain[0], 3u);
	}

	// Query bone 4: nearest cached ancestor is bone 2 → chain=[3,4]
	{
		ChainWalkResult r = cache.FindNearestCachedAncestor(4, skel);
		ASSERT_EQ(r.remainingBones, 2u);
		ASSERT_EQ(r.chain[0], 3u);
		ASSERT_EQ(r.chain[1], 4u);
	}

	// Insert bone 3 into cache (as if we just evaluated it)
	cache.Insert(3, id);

	// Query bone 4 again: nearest ancestor is now bone 3 → chain=[4] (one bone)
	{
		ChainWalkResult r = cache.FindNearestCachedAncestor(4, skel);
		ASSERT_EQ(r.remainingBones, 1u);
		ASSERT_EQ(r.chain[0], 4u);
	}

	// Insert bone 4 — cache should hold all 5 entries
	cache.Insert(4, id);
	ASSERT_EQ(cache.count, 5u);

	// Find(4) must succeed after insertion
	const BoneTransform* found = cache.Find(4);
	ASSERT(found != nullptr);

	// Find(4) must return the transform we inserted (identity)
	ASSERT(NearEq(found->rw, 1.f));
	ASSERT(NearEq(found->rx, 0.f));
}

// -----------------------------------------------------------------------
// Test 3 — AnimNotifyState: double-fire prevention
//
// RecordFire followed by HasFiredThisLoop must gate re-firing within the
// same loop.  Advancing loopStart past the recorded time makes the guard
// transparent (enabling re-fire after a wrap).
// -----------------------------------------------------------------------

TEST(Anim_NotifyState_DoubleFirePrevention)
{
	AnimNotifyState state;
	state.Clear();

	const NotifyID id = TNX_NOTIFY("Footstep");

	// Initially unfired
	ASSERT(!state.HasFiredThisLoop(id, 0, SimFloat{}));

	// Record at t=0.3 in slot 0
	state.RecordFire(id, 0, SimFloat(0.3f));

	// Gated when loopStart <= 0.3
	ASSERT(state.HasFiredThisLoop(id, 0, SimFloat{}));
	ASSERT(state.HasFiredThisLoop(id, 0, SimFloat(0.3f)));

	// Transparent when loopStart > 0.3 (e.g. after wrap to 0.31)
	ASSERT(!state.HasFiredThisLoop(id, 0, SimFloat(0.31f)));

	// Slot 1 must not share slot 0's records
	ASSERT(!state.HasFiredThisLoop(id, 1, SimFloat{}));

	// ClearLoopedRecords(slot, dur) removes records where firedAtTime < dur.
	// Passing 1.f (> 0.3) removes the record; 0.f would remove nothing.
	state.ClearLoopedRecords(0, SimFloat(1.f));
	ASSERT_EQ(state.firedCount, 0u);
	ASSERT(!state.HasFiredThisLoop(id, 0, SimFloat{}));
}

// -----------------------------------------------------------------------
// Test 4 — AnimNotifyState: slot isolation
//
// ClearLoopedRecords(slot) must evict only that slot's records; other
// slots' records remain intact.
// -----------------------------------------------------------------------

TEST(Anim_NotifyState_SlotIsolation)
{
	AnimNotifyState state;
	state.Clear();

	const NotifyID idA = TNX_NOTIFY("NotifyA");
	const NotifyID idB = TNX_NOTIFY("NotifyB");

	state.RecordFire(idA, 0, SimFloat(0.2f));
	state.RecordFire(idB, 1, SimFloat(0.4f));
	ASSERT_EQ(state.firedCount, 2u);

	// Clear slot 0 only (1.f > 0.2, so the slot-0 record is evicted)
	state.ClearLoopedRecords(0, SimFloat(1.f));
	ASSERT_EQ(state.firedCount, 1u);

	// Slot 1 record intact
	ASSERT(state.HasFiredThisLoop(idB, 1, SimFloat{}));
	// Slot 0 record gone
	ASSERT(!state.HasFiredThisLoop(idA, 0, SimFloat{}));
}

// -----------------------------------------------------------------------
// Test 5 — EvaluateRootMotionDelta: delta is correct over a range
//
// Linear root motion track: x goes from 0 to 10 over 1 second.
// Deltas must equal the finite difference over the queried range.
// -----------------------------------------------------------------------

TEST(Anim_RootMotionDelta)
{
	AnimationAsset anim;
	anim.duration  = 1.f;
	anim.boneCount = 0;

	AnimKeyframe k0{};
	k0.time = 0.f;
	k0.tx   = 0.f;
	k0.rw   = 1.f;
	AnimKeyframe k1{};
	k1.time = 1.f;
	k1.tx   = 10.f;
	k1.rw   = 1.f;
	anim.rootMotionTrack.push_back(k0);
	anim.rootMotionTrack.push_back(k1);

	// [0, 0.5) → delta.tx = 5
	BoneTransform d0 = anim.EvaluateRootMotionDelta(0.f, 0.5f);
	ASSERT(NearEq(d0.tx, 5.f));
	ASSERT(NearEq(d0.ty, 0.f));
	ASSERT(NearEq(d0.tz, 0.f));

	// [0.2, 0.7) → delta.tx = 5 (same span, shifted)
	BoneTransform d1 = anim.EvaluateRootMotionDelta(0.2f, 0.7f);
	ASSERT(NearEq(d1.tx, 5.f));

	// [0, 1) → delta.tx = 10 (full clip)
	BoneTransform d2 = anim.EvaluateRootMotionDelta(0.f, 1.f);
	ASSERT(NearEq(d2.tx, 10.f));
}
