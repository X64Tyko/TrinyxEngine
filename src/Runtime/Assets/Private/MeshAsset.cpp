#include "MeshAsset.h"

#include <cstring>
#include <fstream>

#include "Logger.h"

bool SaveMeshAsset(const MeshAsset& asset, const std::string& path)
{
	if (!asset.IsValid())
	{
		LOG_ENG_ERROR("[MeshAsset] Cannot save empty mesh asset");
		return false;
	}

	std::ofstream file(path, std::ios::binary);
	if (!file.is_open())
	{
		LOG_ENG_ERROR_F("[MeshAsset] Failed to open '%s' for writing", path.c_str());
		return false;
	}

	TnxMeshHeader header;
	header.VertexCount = static_cast<uint32_t>(asset.Vertices.size());
	header.IndexCount  = static_cast<uint32_t>(asset.Indices.size());
	std::memcpy(header.AABBMin, asset.AABBMin, sizeof(float) * 3);
	std::memcpy(header.AABBMax, asset.AABBMax, sizeof(float) * 3);
	if (asset.IsSkinned()) header.Flags |= TnxMeshFlag_Skinned;

	file.write(reinterpret_cast<const char*>(&header), sizeof(header));
	file.write(reinterpret_cast<const char*>(asset.Vertices.data()),
		asset.Vertices.size() * sizeof(Vertex));
	file.write(reinterpret_cast<const char*>(asset.Indices.data()),
		asset.Indices.size() * sizeof(uint32_t));
	if (asset.IsSkinned())
		file.write(reinterpret_cast<const char*>(asset.Skin.data()),
			asset.Skin.size() * sizeof(SkinWeights));

	if (!file.good())
	{
		LOG_ENG_ERROR_F("[MeshAsset] Write error for '%s'", path.c_str());
		return false;
	}

	LOG_ENG_INFO_F("[MeshAsset] Saved '%s' (%u verts, %u indices%s)",
		path.c_str(), header.VertexCount, header.IndexCount,
		asset.IsSkinned() ? ", skinned" : "");
	return true;
}

bool LoadMeshAsset(MeshAsset& outAsset, const std::string& path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
	{
		LOG_ENG_ERROR_F("[MeshAsset] Failed to open '%s' for reading", path.c_str());
		return false;
	}

	TnxMeshHeader header;
	file.read(reinterpret_cast<char*>(&header), sizeof(header));

	if (header.Magic != TnxMeshMagic)
	{
		LOG_ENG_ERROR_F("[MeshAsset] Invalid magic in '%s'", path.c_str());
		return false;
	}

	// Version 1 had no Flags field — treat it as no skin data (graceful upgrade).
	// Any other unknown version: warn so the caller can attempt reimport from source.
	if (header.Version != 1 && header.Version != TnxMeshVersion)
	{
		LOG_ENG_WARN_F("[MeshAsset] Stale version %u in '%s' — caller should reimport from source",
			header.Version, path.c_str());
		return false;
	}

	if (header.VertexCount == 0 || header.IndexCount == 0)
	{
		LOG_ENG_ERROR_F("[MeshAsset] Empty mesh in '%s'", path.c_str());
		return false;
	}

	outAsset.Vertices.resize(header.VertexCount);
	outAsset.Indices.resize(header.IndexCount);
	std::memcpy(outAsset.AABBMin, header.AABBMin, sizeof(float) * 3);
	std::memcpy(outAsset.AABBMax, header.AABBMax, sizeof(float) * 3);

	file.read(reinterpret_cast<char*>(outAsset.Vertices.data()),
		header.VertexCount * sizeof(Vertex));
	file.read(reinterpret_cast<char*>(outAsset.Indices.data()),
		header.IndexCount * sizeof(uint32_t));

	const bool skinned = (header.Version >= 2) && (header.Flags & TnxMeshFlag_Skinned);
	if (skinned)
	{
		outAsset.Skin.resize(header.VertexCount);
		file.read(reinterpret_cast<char*>(outAsset.Skin.data()),
			header.VertexCount * sizeof(SkinWeights));
	}

	if (!file.good())
	{
		LOG_ENG_ERROR_F("[MeshAsset] Read error for '%s'", path.c_str());
		outAsset = {};
		return false;
	}

	LOG_ENG_INFO_F("[MeshAsset] Loaded '%s' (%u verts, %u indices%s)",
		path.c_str(), header.VertexCount, header.IndexCount,
		skinned ? ", skinned" : "");
	return true;
}
