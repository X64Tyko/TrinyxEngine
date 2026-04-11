#include "MeshManager.h"
#include "MeshAsset.h"
#include "VertexFormat.h"
#include "Logger.h"

#include <cmath>
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

uint32_t MeshManager::RegisterMesh(const MeshAsset& asset, const std::string& name, AssetID id)
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
	SlotIDs[slotID]   = id;
	if (id.IsValid()) IDToSlot[id.GetUUID()] = slotID;
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

	return RegisterMesh(cube, "Cube");
}

// -----------------------------------------------------------------------
// RegisterBuiltinCapsule — procedural capsule (two hemispheres + cylinder)
// -----------------------------------------------------------------------

uint32_t MeshManager::RegisterBuiltinCapsule(float radius, float halfHeight, uint32_t segments)
{
	constexpr float PI = 3.14159265358979323846f;

	// Ring count for each hemisphere (latitude subdivisions)
	const uint32_t hemiRings = segments / 4;
	if (hemiRings < 2) return UINT32_MAX;

	// Total latitude rings: top hemisphere + cylinder equator (2 rings) + bottom hemisphere
	// Vertices: (segments+1) per ring, plus 2 pole vertices
	const uint32_t slices = segments; // longitude subdivisions

	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;

	auto pushVert = [&](float px, float py, float pz, float nx, float ny, float nz)
	{
		Vertex v{};
		v.px        = px;
		v.py        = py;
		v.pz        = pz;
		v.n_oct16x2 = OctEncode(nx, ny, nz);
		v.u         = 0.0f;
		v.v         = 0.0f;
		v.t_oct16x2 = OctEncode(0.0f, 0.0f, 1.0f);
		v.mask      = 0;
		v.flags     = 0;
		v.pad       = 0;
		vertices.push_back(v);
	};

	// --- Top pole ---
	pushVert(0.0f, halfHeight + radius, 0.0f, 0.0f, 1.0f, 0.0f);

	// --- Top hemisphere rings (from pole down to equator) ---
	for (uint32_t ring = 1; ring <= hemiRings; ++ring)
	{
		float phi = (PI * 0.5f) * (1.0f - static_cast<float>(ring) / static_cast<float>(hemiRings));
		float cy  = sinf(phi) * radius + halfHeight; // center Y offset
		float cr  = cosf(phi) * radius;              // ring radius

		float ny = sinf(phi);
		float nr = cosf(phi);

		for (uint32_t s = 0; s <= slices; ++s)
		{
			float theta = 2.0f * PI * static_cast<float>(s) / static_cast<float>(slices);
			float nx    = nr * cosf(theta);
			float nz    = nr * sinf(theta);
			pushVert(cr * cosf(theta), cy, cr * sinf(theta), nx, ny, nz);
		}
	}

	// --- Bottom hemisphere rings (from equator down to pole) ---
	for (uint32_t ring = 1; ring <= hemiRings; ++ring)
	{
		float phi = -(PI * 0.5f) * static_cast<float>(ring) / static_cast<float>(hemiRings);
		float cy  = sinf(phi) * radius - halfHeight;
		float cr  = cosf(phi) * radius;

		float ny = sinf(phi);
		float nr = cosf(phi);

		for (uint32_t s = 0; s <= slices; ++s)
		{
			float theta = 2.0f * PI * static_cast<float>(s) / static_cast<float>(slices);
			float nx    = nr * cosf(theta);
			float nz    = nr * sinf(theta);
			pushVert(cr * cosf(theta), cy, cr * sinf(theta), nx, ny, nz);
		}
	}

	// --- Bottom pole ---
	pushVert(0.0f, -(halfHeight + radius), 0.0f, 0.0f, -1.0f, 0.0f);

	uint32_t bottomPole = static_cast<uint32_t>(vertices.size()) - 1;
	uint32_t totalRings = hemiRings * 2; // top hemi rings + bottom hemi rings

	// --- Indices: top pole fan ---
	for (uint32_t s = 0; s < slices; ++s)
	{
		indices.push_back(0);         // top pole
		indices.push_back(1 + s);     // current ring vertex
		indices.push_back(1 + s + 1); // next ring vertex
	}

	// --- Indices: ring strips ---
	for (uint32_t ring = 0; ring < totalRings - 1; ++ring)
	{
		uint32_t ringStart     = 1 + ring * (slices + 1);
		uint32_t nextRingStart = 1 + (ring + 1) * (slices + 1);

		for (uint32_t s = 0; s < slices; ++s)
		{
			uint32_t a = ringStart + s;
			uint32_t b = ringStart + s + 1;
			uint32_t c = nextRingStart + s;
			uint32_t d = nextRingStart + s + 1;

			indices.push_back(a);
			indices.push_back(b);
			indices.push_back(c);

			indices.push_back(b);
			indices.push_back(d);
			indices.push_back(c);
		}
	}

	// --- Indices: bottom pole fan ---
	uint32_t lastRingStart = 1 + (totalRings - 1) * (slices + 1);
	for (uint32_t s = 0; s < slices; ++s)
	{
		indices.push_back(bottomPole);
		indices.push_back(lastRingStart + s + 1);
		indices.push_back(lastRingStart + s);
	}

	MeshAsset capsule;
	capsule.Vertices   = std::move(vertices);
	capsule.Indices    = std::move(indices);
	capsule.AABBMin[0] = -radius;
	capsule.AABBMin[1] = -(halfHeight + radius);
	capsule.AABBMin[2] = -radius;
	capsule.AABBMax[0] = radius;
	capsule.AABBMax[1] = halfHeight + radius;
	capsule.AABBMax[2] = radius;

	return RegisterMesh(capsule, "Capsule");
}
