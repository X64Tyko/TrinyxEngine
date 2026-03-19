#pragma once
#include <cstdint>
#include <string>

#include "GpuFrameData.h"
#include "VulkanMemory.h"

struct MeshAsset;

// -----------------------------------------------------------------------
// MeshManager — GPU mega-buffer management for all mesh geometry.
//
// All meshes are sub-allocated into a single vertex mega-buffer and a
// single index mega-buffer.  Each registered mesh gets a MeshSlot
// describing its region.  The vertex shader reads vertices via BDA;
// the index buffer is bound via vkCmdBindIndexBuffer.
//
// A GPU-side MeshTable buffer (GpuMeshInfo[MAX_MESH_SLOTS]) is maintained
// in sync with CPU slots for the build_draws compute pass.
//
// Slot 0 is always the built-in cube mesh.
// -----------------------------------------------------------------------

class MeshManager
{
public:
	static constexpr uint32_t MAX_MESH_SLOTS          = 256;
	static constexpr uint32_t VERTEX_MEGA_BUFFER_SIZE = 16 * 1024 * 1024; // 16 MB
	static constexpr uint32_t INDEX_MEGA_BUFFER_SIZE  = 4 * 1024 * 1024;  //  4 MB

	struct MeshSlot
	{
		uint32_t FirstIndex  = 0;
		uint32_t IndexCount  = 0;
		int32_t VertexOffset = 0; // signed per Vulkan spec
		float AABBMin[3]     = {};
		float AABBMax[3]     = {};
	};

	bool Initialize(VulkanMemory* vkMem);

	/// Register a MeshAsset — copies vertex/index data into the mega-buffers.
	/// Returns the slot ID, or UINT32_MAX on failure.
	uint32_t RegisterMesh(const MeshAsset& asset, const std::string& name = {});

	/// Register the built-in cube mesh as slot 0.
	uint32_t RegisterBuiltinCube();

	uint64_t GetVertexBufferAddr() const { return VertexMegaBuffer.DeviceAddr; }
	VkBuffer GetIndexBufferHandle() const { return static_cast<VkBuffer>(IndexMegaBuffer.Buffer); }
	uint64_t GetMeshTableAddr() const { return MeshTableBuffer.DeviceAddr; }
	const MeshSlot& GetSlot(uint32_t id) const { return Slots[id]; }
	uint32_t GetMeshCount() const { return MeshCount; }

	/// Find a mesh slot by name. Returns UINT32_MAX if not found.
	uint32_t FindSlotByName(const std::string& name) const
	{
		for (uint32_t i = 0; i < MeshCount; ++i) if (SlotNames[i] == name) return i;
		return UINT32_MAX;
	}

	const std::string& GetSlotName(uint32_t id) const { return SlotNames[id]; }

private:
	VulkanBuffer VertexMegaBuffer;
	VulkanBuffer IndexMegaBuffer;
	VulkanBuffer MeshTableBuffer; // GpuMeshInfo[MAX_MESH_SLOTS], PersistentMapped + BDA

	uint32_t NextVertexOffset = 0; // in vertices (not bytes)
	uint32_t NextIndexOffset  = 0; // in indices  (not bytes)
	uint32_t MeshCount        = 0;
	MeshSlot Slots[MAX_MESH_SLOTS]{};
	std::string SlotNames[MAX_MESH_SLOTS];
};
