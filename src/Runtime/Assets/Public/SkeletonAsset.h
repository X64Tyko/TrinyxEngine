#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "AnimTypes.h"
#include "TnxName.h"

// -----------------------------------------------------------------------
// .tnxskel binary format
//
// 64-byte header:
//   Offset  Size  Field
//   0       4     Magic: "SKEL" (0x4C454B53)
//   4       4     Version: 1
//   8       4     BoneCount
//   12      4     SocketCount
//   16      48    Reserved (pad to 64)
//   64      B     BoneInfo[BoneCount]  (156 bytes each)
//   64+B    S     SocketInfo[SocketCount]  (variable, see below)
// -----------------------------------------------------------------------

constexpr uint32_t TnxSkelMagic   = 0x4C454B53; // "SKEL"
constexpr uint32_t TnxSkelVersion = 1;

struct TnxSkelHeader
{
	uint32_t Magic       = TnxSkelMagic;
	uint32_t Version     = TnxSkelVersion;
	uint32_t BoneCount   = 0;
	uint32_t SocketCount = 0;
	uint8_t Reserved[48]{};
};

static_assert(sizeof(TnxSkelHeader) == 64, "TnxSkelHeader must be exactly 64 bytes");

// -----------------------------------------------------------------------
// On-disk bone record.  Name hash used at runtime; string for editor only.
// -----------------------------------------------------------------------

struct BoneInfoDisk
{
	uint32_t parentIndex;      // 0xFFFFFFFF for root
	float inverseBindPose[16]; // column-major mat4  (64B)
	uint32_t nameHash;         // TnxName::Value — used at runtime
	char nameStr[28];          // stripped in TNX_SHIPPING builds (pad to 32B)
};

static_assert(sizeof(BoneInfoDisk) == 4 + 64 + 4 + 28, "BoneInfoDisk unexpected size");

// -----------------------------------------------------------------------
// On-disk socket record.  id is a TnxName hash — the SocketID.
// -----------------------------------------------------------------------

struct SocketInfoDisk
{
	uint32_t id; // TnxName hash (== SocketID)
	uint32_t boneIndex;
	// BoneTransform stored as raw floats — disk format is always float regardless of SimFloat mode.
	float tx, ty, tz;
	float rx, ry, rz, rw;
	float sx, sy, sz;
	uint8_t prewarm;  // bool
	char nameStr[27]; // editor-only string (pad to 32B total after prewarm)
};

static_assert(sizeof(SocketInfoDisk) == 4 + 4 + 40 + 1 + 27, "SocketInfoDisk unexpected size");

// -----------------------------------------------------------------------
// In-memory skeleton.  Loaded from .tnxskel, kept by SkeletonManager.
// -----------------------------------------------------------------------

struct BoneInfo
{
	uint32_t parentIndex;
	float inverseBindPose[16];
	TnxName name;
};

struct SocketDef
{
	SocketID id; // TnxName::Value — compare with TNX_SOCKET("LeftFoot")
	uint32_t boneIndex;
	BoneTransform localOffset;
	bool prewarm;
	TnxName name;
};

struct SkeletonAsset
{
	uint32_t boneCount   = 0;
	uint32_t socketCount = 0;
	std::vector<BoneInfo> bones;
	std::vector<SocketDef> sockets;

	// ----------------------------------------------------------------
	// Hierarchy queries — used by BoneCacheLocal::FindNearestCachedAncestor
	// ----------------------------------------------------------------

	uint32_t GetParent(uint32_t boneIndex) const
	{
		return (boneIndex < boneCount) ? bones[boneIndex].parentIndex : 0xFFFFFFFF;
	}

	// Fill outChain[0..outLen) with bone indices from root to boneIndex (inclusive).
	void GetChainFromRoot(uint32_t boneIndex, uint32_t* outChain, uint32_t& outLen) const;

	bool IsAncestor(uint32_t ancestor, uint32_t descendant) const;

	// Returns the SocketDef for id, or nullptr if not registered.
	const SocketDef* FindSocket(SocketID id) const
	{
		for (const SocketDef& s : sockets)
			if (s.id == id) return &s;
		return nullptr;
	}

	// Returns bone index for a TnxName, or 0xFFFFFFFF if not found.
	uint32_t FindBone(TnxName name) const
	{
		for (uint32_t i = 0; i < boneCount; ++i)
			if (bones[i].name == name) return i;
		return 0xFFFFFFFF;
	}
};

bool SaveSkeletonAsset(const SkeletonAsset& asset, const std::string& path);
bool LoadSkeletonAsset(SkeletonAsset& outAsset, const std::string& path);
