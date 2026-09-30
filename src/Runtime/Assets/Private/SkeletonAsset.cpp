#include "SkeletonAsset.h"
#include "Logger.h"

#include <cstring>
#include <fstream>

// -----------------------------------------------------------------------
// SkeletonAsset — hierarchy queries
// -----------------------------------------------------------------------

void SkeletonAsset::GetChainFromRoot(uint32_t boneIndex, uint32_t* outChain, uint32_t& outLen) const
{
	outLen = 0;
	if (boneIndex >= boneCount) return;

	// Walk up to root, collecting indices
	uint32_t tmp[256];
	uint32_t depth = 0;
	uint32_t cur   = boneIndex;
	while (cur != 0xFFFFFFFF && depth < 256)
	{
		tmp[depth++] = cur;
		cur          = GetParent(cur);
	}

	// Reverse so [0] == root
	outLen = depth;
	for (uint32_t i = 0; i < depth; ++i)
		outChain[i] = tmp[depth - 1 - i];
}

bool SkeletonAsset::IsAncestor(uint32_t ancestor, uint32_t descendant) const
{
	uint32_t cur = GetParent(descendant);
	while (cur != 0xFFFFFFFF)
	{
		if (cur == ancestor) return true;
		cur = GetParent(cur);
	}
	return false;
}

// -----------------------------------------------------------------------
// Save / Load
// -----------------------------------------------------------------------

bool SaveSkeletonAsset(const SkeletonAsset& asset, const std::string& path)
{
	std::ofstream file(path, std::ios::binary);
	if (!file.is_open())
	{
		LOG_ENG_ERROR_F("[SkeletonAsset] Failed to open '%s' for writing", path.c_str());
		return false;
	}

	TnxSkelHeader header;
	header.BoneCount   = asset.boneCount;
	header.SocketCount = asset.socketCount;
	file.write(reinterpret_cast<const char*>(&header), sizeof(header));

	for (const BoneInfo& b : asset.bones)
	{
		BoneInfoDisk disk;
		disk.parentIndex = b.parentIndex;
		std::memcpy(disk.inverseBindPose, b.inverseBindPose, sizeof(disk.inverseBindPose));
		disk.nameHash = b.name.Value;
		std::memset(disk.nameStr, 0, sizeof(disk.nameStr));
#ifndef TNX_STRIP_NAMES
		std::strncpy(disk.nameStr, b.name.GetStr(), sizeof(disk.nameStr) - 1);
#endif
		file.write(reinterpret_cast<const char*>(&disk), sizeof(disk));
	}

	for (const SocketDef& s : asset.sockets)
	{
		SocketInfoDisk disk;
		disk.id        = s.id;
		disk.boneIndex = s.boneIndex;
		disk.tx        = s.localOffset.tx.ToFloat();
		disk.ty        = s.localOffset.ty.ToFloat();
		disk.tz        = s.localOffset.tz.ToFloat();
		disk.rx        = s.localOffset.rx.ToFloat();
		disk.ry        = s.localOffset.ry.ToFloat();
		disk.rz        = s.localOffset.rz.ToFloat();
		disk.rw        = s.localOffset.rw.ToFloat();
		disk.sx        = s.localOffset.sx.ToFloat();
		disk.sy        = s.localOffset.sy.ToFloat();
		disk.sz        = s.localOffset.sz.ToFloat();
		disk.prewarm   = s.prewarm ? 1 : 0;
		std::memset(disk.nameStr, 0, sizeof(disk.nameStr));
#ifndef TNX_STRIP_NAMES
		std::strncpy(disk.nameStr, s.name.GetStr(), sizeof(disk.nameStr) - 1);
#endif
		file.write(reinterpret_cast<const char*>(&disk), sizeof(disk));
	}

	if (!file.good())
	{
		LOG_ENG_ERROR_F("[SkeletonAsset] Write error for '%s'", path.c_str());
		return false;
	}

	LOG_ENG_INFO_F("[SkeletonAsset] Saved '%s' (%u bones, %u sockets)",
		path.c_str(), asset.boneCount, asset.socketCount);
	return true;
}

bool LoadSkeletonAsset(SkeletonAsset& outAsset, const std::string& path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
	{
		LOG_ENG_ERROR_F("[SkeletonAsset] Failed to open '%s' for reading", path.c_str());
		return false;
	}

	TnxSkelHeader header;
	file.read(reinterpret_cast<char*>(&header), sizeof(header));

	if (header.Magic != TnxSkelMagic)
	{
		LOG_ENG_ERROR_F("[SkeletonAsset] Invalid magic in '%s'", path.c_str());
		return false;
	}
	if (header.Version != TnxSkelVersion)
	{
		LOG_ENG_ERROR_F("[SkeletonAsset] Unsupported version %u in '%s'", header.Version, path.c_str());
		return false;
	}

	outAsset.boneCount   = header.BoneCount;
	outAsset.socketCount = header.SocketCount;
	outAsset.bones.resize(header.BoneCount);
	outAsset.sockets.resize(header.SocketCount);

	for (BoneInfo& b : outAsset.bones)
	{
		BoneInfoDisk disk;
		file.read(reinterpret_cast<char*>(&disk), sizeof(disk));
		b.parentIndex = disk.parentIndex;
		std::memcpy(b.inverseBindPose, disk.inverseBindPose, sizeof(b.inverseBindPose));
		b.name = TnxName(disk.nameHash, disk.nameStr);
	}

	for (SocketDef& s : outAsset.sockets)
	{
		SocketInfoDisk disk;
		file.read(reinterpret_cast<char*>(&disk), sizeof(disk));
		s.id             = disk.id;
		s.boneIndex      = disk.boneIndex;
		s.localOffset.tx = disk.tx;
		s.localOffset.ty = disk.ty;
		s.localOffset.tz = disk.tz;
		s.localOffset.rx = disk.rx;
		s.localOffset.ry = disk.ry;
		s.localOffset.rz = disk.rz;
		s.localOffset.rw = disk.rw;
		s.localOffset.sx = disk.sx;
		s.localOffset.sy = disk.sy;
		s.localOffset.sz = disk.sz;
		s.prewarm        = disk.prewarm != 0;
		s.name           = TnxName(disk.id, disk.nameStr);
	}

	if (!file.good())
	{
		LOG_ENG_ERROR_F("[SkeletonAsset] Read error for '%s'", path.c_str());
		outAsset = {};
		return false;
	}

	LOG_ENG_INFO_F("[SkeletonAsset] Loaded '%s' (%u bones, %u sockets)",
		path.c_str(), header.BoneCount, header.SocketCount);
	return true;
}
