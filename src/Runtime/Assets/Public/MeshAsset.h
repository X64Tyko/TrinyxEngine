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
//   4       4     Version: 2
//   8       4     VertexCount (uint32)
//   12      4     IndexCount  (uint32)
//   16      12    AABB min    (float3)
//   28      12    AABB max    (float3)
//   40      4     Flags       (TnxMeshFlag_*)
//   44      20    Reserved    (pad to 64-byte header)
//   64      V     Vertices: VertexCount × 32 bytes
//   64+V    I     Indices:  IndexCount  × 4 bytes (uint32)
//   [if Skinned flag set]
//   64+V+I  S     SkinWeights: VertexCount × 8 bytes
// -----------------------------------------------------------------------

constexpr uint32_t TnxMeshMagic   = 0x4D584E54; // "TNXM" little-endian
constexpr uint32_t TnxMeshVersion = 2;

constexpr uint32_t TnxMeshFlag_Skinned = 0x1; // mesh carries parallel SkinWeights data

struct TnxMeshHeader
{
	uint32_t Magic       = TnxMeshMagic;
	uint32_t Version     = TnxMeshVersion;
	uint32_t VertexCount = 0;
	uint32_t IndexCount  = 0;
	float AABBMin[3]     = {};
	float AABBMax[3]     = {};
	uint32_t Flags       = 0;
	uint8_t Reserved[20] = {};
};

static_assert(sizeof(TnxMeshHeader) == 64, "TnxMeshHeader must be exactly 64 bytes");

// -----------------------------------------------------------------------
// MeshAsset — in-memory representation of an imported mesh.
// Skin is parallel to Vertices — one entry per vertex, same order.
// -----------------------------------------------------------------------

struct MeshAsset
{
	std::vector<Vertex> Vertices;
	std::vector<uint32_t> Indices;
	std::vector<SkinWeights> Skin; // empty for static meshes
	float AABBMin[3] = {};
	float AABBMax[3] = {};

	bool IsSkinned() const { return !Skin.empty(); }
	bool IsValid() const { return !Vertices.empty() && !Indices.empty(); }
};

// -----------------------------------------------------------------------
// Binary read/write
// -----------------------------------------------------------------------

bool SaveMeshAsset(const MeshAsset& asset, const std::string& path);
bool LoadMeshAsset(MeshAsset& outAsset, const std::string& path);
