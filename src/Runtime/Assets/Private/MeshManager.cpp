#include "MeshManager.h"
#include "MeshAsset.h"
#include "VertexFormat.h"
#include "Logger.h"

#include <cstring>

// -----------------------------------------------------------------------
// Built-in cube geometry (new Vertex format)
//
// 24 vertices (4 per face for flat normals), 36 uint32 indices.
// Replaces the old CubeMesh namespace which used float4+float4 layout.
// -----------------------------------------------------------------------

namespace BuiltinCube
{
	static Vertex MakeVert(float px, float py, float pz, float nx, float ny, float nz)
	{
		Vertex v{};
		v.px        = px;
		v.py        = py;
		v.pz        = pz;
		v.n_oct16x2 = OctEncode(nx, ny, nz);
		v.u         = 0.0f;
		v.v         = 0.0f;
		v.t_oct16x2 = OctEncode(0.0f, 0.0f, 1.0f); // default tangent +Z
		v.mask      = 0;
		v.flags     = 0;
		v.pad       = 0;
		return v;
	}

	static const Vertex Vertices[] = {
		// +Z front face (normal 0,0,1)
		MakeVert(-0.5f, -0.5f, 0.5f, 0, 0, 1),
		MakeVert(0.5f, -0.5f, 0.5f, 0, 0, 1),
		MakeVert(0.5f, 0.5f, 0.5f, 0, 0, 1),
		MakeVert(-0.5f, 0.5f, 0.5f, 0, 0, 1),
		// -Z back face (normal 0,0,-1)
		MakeVert(0.5f, -0.5f, -0.5f, 0, 0, -1),
		MakeVert(-0.5f, -0.5f, -0.5f, 0, 0, -1),
		MakeVert(-0.5f, 0.5f, -0.5f, 0, 0, -1),
		MakeVert(0.5f, 0.5f, -0.5f, 0, 0, -1),
		// +X right face (normal 1,0,0)
		MakeVert(0.5f, -0.5f, 0.5f, 1, 0, 0),
		MakeVert(0.5f, -0.5f, -0.5f, 1, 0, 0),
		MakeVert(0.5f, 0.5f, -0.5f, 1, 0, 0),
		MakeVert(0.5f, 0.5f, 0.5f, 1, 0, 0),
		// -X left face (normal -1,0,0)
		MakeVert(-0.5f, -0.5f, -0.5f, -1, 0, 0),
		MakeVert(-0.5f, -0.5f, 0.5f, -1, 0, 0),
		MakeVert(-0.5f, 0.5f, 0.5f, -1, 0, 0),
		MakeVert(-0.5f, 0.5f, -0.5f, -1, 0, 0),
		// +Y top face (normal 0,1,0)
		MakeVert(-0.5f, 0.5f, 0.5f, 0, 1, 0),
		MakeVert(0.5f, 0.5f, 0.5f, 0, 1, 0),
		MakeVert(0.5f, 0.5f, -0.5f, 0, 1, 0),
		MakeVert(-0.5f, 0.5f, -0.5f, 0, 1, 0),
		// -Y bottom face (normal 0,-1,0)
		MakeVert(-0.5f, -0.5f, -0.5f, 0, -1, 0),
		MakeVert(0.5f, -0.5f, -0.5f, 0, -1, 0),
		MakeVert(0.5f, -0.5f, 0.5f, 0, -1, 0),
		MakeVert(-0.5f, -0.5f, 0.5f, 0, -1, 0),
	};

	static constexpr uint32_t Indices[] = {
		0, 1, 2, 2, 3, 0,       // +Z front
		4, 5, 6, 6, 7, 4,       // -Z back
		8, 9, 10, 10, 11, 8,    // +X right
		12, 13, 14, 14, 15, 12, // -X left
		16, 17, 18, 18, 19, 16, // +Y top
		20, 21, 22, 22, 23, 20, // -Y bottom
	};

	static constexpr uint32_t VertexCount = 24;
	static constexpr uint32_t IndexCount  = 36;
}

// -----------------------------------------------------------------------
// Initialize
// -----------------------------------------------------------------------

bool MeshManager::Initialize(VulkanMemory* vkMem)
{
	VertexMegaBuffer = vkMem->AllocateBuffer(
		VERTEX_MEGA_BUFFER_SIZE,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		GpuMemoryDomain::PersistentMapped,
		/*requestDeviceAddress=*/ true);

	if (!VertexMegaBuffer.IsValid())
	{
		LOG_ERROR("[MeshManager] Vertex mega-buffer allocation failed");
		return false;
	}

	IndexMegaBuffer = vkMem->AllocateBuffer(
		INDEX_MEGA_BUFFER_SIZE,
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
		GpuMemoryDomain::PersistentMapped,
		/*requestDeviceAddress=*/ false);

	if (!IndexMegaBuffer.IsValid())
	{
		LOG_ERROR("[MeshManager] Index mega-buffer allocation failed");
		return false;
	}

	MeshTableBuffer = vkMem->AllocateBuffer(
		MAX_MESH_SLOTS * sizeof(GpuMeshInfo),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		GpuMemoryDomain::PersistentMapped,
		/*requestDeviceAddress=*/ true);

	if (!MeshTableBuffer.IsValid())
	{
		LOG_ERROR("[MeshManager] Mesh table buffer allocation failed");
		return false;
	}
	std::memset(MeshTableBuffer.MappedPtr, 0, MAX_MESH_SLOTS * sizeof(GpuMeshInfo));

	LOG_INFO_F("[MeshManager] Initialized (vertex: %u MB, index: %u MB)",
			   VERTEX_MEGA_BUFFER_SIZE / (1024 * 1024),
			   INDEX_MEGA_BUFFER_SIZE / (1024 * 1024));
	return true;
}

// -----------------------------------------------------------------------
// RegisterMesh
// -----------------------------------------------------------------------

uint32_t MeshManager::RegisterMesh(const MeshAsset& asset, const std::string& name)
{
	if (MeshCount >= MAX_MESH_SLOTS)
	{
		LOG_ERROR("[MeshManager] Mesh slot limit reached");
		return UINT32_MAX;
	}

	const uint32_t vertBytes  = static_cast<uint32_t>(asset.Vertices.size()) * sizeof(Vertex);
	const uint32_t indexBytes = static_cast<uint32_t>(asset.Indices.size()) * sizeof(uint32_t);

	if ((NextVertexOffset * sizeof(Vertex) + vertBytes) > VERTEX_MEGA_BUFFER_SIZE)
	{
		LOG_ERROR("[MeshManager] Vertex mega-buffer overflow");
		return UINT32_MAX;
	}
	if ((NextIndexOffset * sizeof(uint32_t) + indexBytes) > INDEX_MEGA_BUFFER_SIZE)
	{
		LOG_ERROR("[MeshManager] Index mega-buffer overflow");
		return UINT32_MAX;
	}

	// Copy data into mega-buffers
	auto* vertDst = static_cast<uint8_t*>(VertexMegaBuffer.MappedPtr)
		+ NextVertexOffset * sizeof(Vertex);
	auto* idxDst = static_cast<uint8_t*>(IndexMegaBuffer.MappedPtr)
		+ NextIndexOffset * sizeof(uint32_t);

	std::memcpy(vertDst, asset.Vertices.data(), vertBytes);
	std::memcpy(idxDst, asset.Indices.data(), indexBytes);

	// Fill slot
	uint32_t slotID   = MeshCount++;
	SlotNames[slotID] = name;
	MeshSlot& slot    = Slots[slotID];
	slot.FirstIndex   = NextIndexOffset;
	slot.IndexCount   = static_cast<uint32_t>(asset.Indices.size());
	slot.VertexOffset = static_cast<int32_t>(NextVertexOffset);
	std::memcpy(slot.AABBMin, asset.AABBMin, sizeof(float) * 3);
	std::memcpy(slot.AABBMax, asset.AABBMax, sizeof(float) * 3);

	// Update GPU-side mesh table
	auto* gpuTable                = static_cast<GpuMeshInfo*>(MeshTableBuffer.MappedPtr);
	gpuTable[slotID].FirstIndex   = slot.FirstIndex;
	gpuTable[slotID].IndexCount   = slot.IndexCount;
	gpuTable[slotID].VertexOffset = slot.VertexOffset;
	gpuTable[slotID]._pad         = 0;

	NextVertexOffset += static_cast<uint32_t>(asset.Vertices.size());
	NextIndexOffset  += static_cast<uint32_t>(asset.Indices.size());

	LOG_INFO_F("[MeshManager] Registered mesh slot %u (%zu verts, %zu indices)",
			   slotID, asset.Vertices.size(), asset.Indices.size());
	return slotID;
}

// -----------------------------------------------------------------------
// RegisterBuiltinCube — always slot 0
// -----------------------------------------------------------------------

uint32_t MeshManager::RegisterBuiltinCube()
{
	MeshAsset cube;
	cube.Vertices.assign(BuiltinCube::Vertices,
						 BuiltinCube::Vertices + BuiltinCube::VertexCount);
	cube.Indices.assign(BuiltinCube::Indices,
						BuiltinCube::Indices + BuiltinCube::IndexCount);
	cube.AABBMin[0] = -0.5f;
	cube.AABBMin[1] = -0.5f;
	cube.AABBMin[2] = -0.5f;
	cube.AABBMax[0] = 0.5f;
	cube.AABBMax[1] = 0.5f;
	cube.AABBMax[2] = 0.5f;

	RegisterMesh(cube, "Cube1");

	return RegisterMesh(cube, "Cube2");
}
