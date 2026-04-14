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

	file.write(reinterpret_cast<const char*>(&header), sizeof(header));
	file.write(reinterpret_cast<const char*>(asset.Vertices.data()),
			   asset.Vertices.size() * sizeof(Vertex));
	file.write(reinterpret_cast<const char*>(asset.Indices.data()),
			   asset.Indices.size() * sizeof(uint32_t));

	if (!file.good())
	{
		LOG_ENG_ERROR_F("[MeshAsset] Write error for '%s'", path.c_str());
		return false;
	}

	LOG_ENG_INFO_F("[MeshAsset] Saved '%s' (%u verts, %u indices)",
				   path.c_str(), header.VertexCount, header.IndexCount);
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

	if (header.Version != TnxMeshVersion)
	{
		LOG_ENG_ERROR_F("[MeshAsset] Unsupported version %u in '%s'", header.Version, path.c_str());
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

	if (!file.good())
	{
		LOG_ENG_ERROR_F("[MeshAsset] Read error for '%s'", path.c_str());
		outAsset = {};
		return false;
	}

	LOG_ENG_INFO_F("[MeshAsset] Loaded '%s' (%u verts, %u indices)",
				   path.c_str(), header.VertexCount, header.IndexCount);
	return true;
}