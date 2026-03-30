#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "VertexFormat.h"

// -----------------------------------------------------------------------
// .tnxmesh binary format
//
// 64-byte header (one cache line, mmap-friendly):
//   Offset  Size  Field
//   0       4     Magic: "TNXM" (0x4D584E54)
//   4       4     Version: 1
//   8       4     VertexCount (uint32)
//   12      4     IndexCount  (uint32)
//   16      12    AABB min    (float3)
//   28      12    AABB max    (float3)
//   40      24    Reserved    (pad to 64-byte header)
//   64      V     Vertices: VertexCount × 32 bytes
//   64+V    I     Indices:  IndexCount  × 4 bytes (uint32)
// -----------------------------------------------------------------------

constexpr uint32_t TnxMeshMagic   = 0x4D584E54; // "TNXM" little-endian
constexpr uint32_t TnxMeshVersion = 1;

struct TnxMeshHeader
{
	uint32_t Magic       = TnxMeshMagic;
	uint32_t Version     = TnxMeshVersion;
	uint32_t VertexCount = 0;
	uint32_t IndexCount  = 0;
	float AABBMin[3]     = {};
	float AABBMax[3]     = {};
	uint8_t Reserved[24] = {};
};

static_assert(sizeof(TnxMeshHeader) == 64, "TnxMeshHeader must be exactly 64 bytes");

// -----------------------------------------------------------------------
// MeshAsset — in-memory representation of an imported mesh.
// -----------------------------------------------------------------------

struct MeshAsset
{
	std::vector<Vertex> Vertices;
	std::vector<uint32_t> Indices;
	float AABBMin[3] = {};
	float AABBMax[3] = {};

	bool IsValid() const { return !Vertices.empty() && !Indices.empty(); }
};

// -----------------------------------------------------------------------
// Binary read/write
// -----------------------------------------------------------------------

bool SaveMeshAsset(const MeshAsset& asset, const std::string& path);
bool LoadMeshAsset(MeshAsset& outAsset, const std::string& path);