#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "AnimTypes.h"

// -----------------------------------------------------------------------
// .tnxanim binary format  (version 1 — linear keyframes)
//
// 64-byte header:
//   Offset  Size  Field
//   0       4     Magic: "ANIM" (0x4D494E41)
//   4       4     Version: 1  (M3: bump to 2 for CurveHandle compression)
//   8       4     BoneCount
//   12      4     KeyframeCount  (total across all bone tracks)
//   16      4     Duration (float seconds)
//   20      4     NotifyCount  (M2: populated; M1: 0)
//   24      1     HasRootMotion (M2: populated; M1: 0)
//   25      39    Reserved (pad to 64)
//   64      T     AnimBoneTrackDisk[BoneCount]  (8 bytes each)
//   64+T    K     AnimKeyframeDisk[KeyframeCount]  (32 bytes each)
//   64+T+K  N     AnimNotifyDisk[NotifyCount]  (M2+, 48 bytes each)
// -----------------------------------------------------------------------

constexpr uint32_t TnxAnimMagic   = 0x4D494E41; // "ANIM"
constexpr uint32_t TnxAnimVersion = 1;

struct TnxAnimHeader
{
	uint32_t Magic         = TnxAnimMagic;
	uint32_t Version       = TnxAnimVersion;
	uint32_t BoneCount     = 0;
	uint32_t KeyframeCount = 0;
	float Duration         = 0.f;
	uint32_t NotifyCount   = 0;
	uint8_t HasRootMotion  = 0;
	uint8_t Reserved[39]{};
};

static_assert(sizeof(TnxAnimHeader) == 64, "TnxAnimHeader must be exactly 64 bytes");

struct AnimBoneTrackDisk
{
	uint32_t keyframeOffset;
	uint32_t keyframeCount;
};

static_assert(sizeof(AnimBoneTrackDisk) == 8);

struct AnimKeyframeDisk // scale stored as identity (M1)
{
	float time;
	float tx, ty, tz;
	float rx, ry, rz, rw;
};

static_assert(sizeof(AnimKeyframeDisk) == 32);

struct AnimNotifyDisk // M2+
{
	uint32_t idHash;
	float triggerTime;
	float duration; // 0 = instant, >0 = state notify
	uint32_t nameHash;
	char nameStr[32]; // editor only
};

static_assert(sizeof(AnimNotifyDisk) == 48);

struct AnimBoneTrack
{
	uint32_t keyframeOffset;
	uint32_t keyframeCount;
};

struct AnimKeyframe
{
	float time;
	float tx, ty, tz;
	float rx, ry, rz, rw;
};

struct AnimNotifyDef
{
	NotifyID id;
	float triggerTime;
	float duration; // 0 = instant, >0 = state notify
	TnxName name;
};

/// Runtime animation clip loaded from a .tnxanim file.
/// Holds per-bone keyframe tracks plus optional notify and root-motion data (M2+).
struct AnimationAsset
{
	float duration     = 0.f;
	uint32_t boneCount = 0;

	std::vector<AnimBoneTrack> boneTracks;
	std::vector<AnimKeyframe> keyframes;

	std::vector<AnimNotifyDef> notifies;       // M2+
	bool hasRootMotion = false;                // M2+
	std::vector<AnimKeyframe> rootMotionTrack; // M2+; bone-0 extracted from main tracks

	/// M1: linear keyframe interp. M3: replaced by CurveHandle evaluation.
	BoneTransform EvaluateBone(uint32_t boneIndex, float timestamp) const;

	/// Returns the root-motion translation delta over [fromTime, toTime]. M2+.
	BoneTransform EvaluateRootMotionDelta(float fromTime, float toTime) const;
};

bool SaveAnimationAsset(const AnimationAsset& asset, const std::string& path);
bool LoadAnimationAsset(AnimationAsset& outAsset, const std::string& path);
