#pragma once
#include <cstdint>
#include <string>

#include "AssetRegistry.h"
#include "AssetTypes.h"
#include "GpuFrameData.h"
#include "VulkanMemory.h"

struct MeshAsset;

// -----------------------------------------------------------------------
// BuiltinMesh — fixed AssetIDs for engine built-in meshes.
//
// AssetDatabase generates UUIDs as (counter << 8) where counter is uint32_t,
// so all AssetDatabase UUIDs fit in bits [39:8]. Builtins use bits [47:40]
// (values >= 2^40) to guarantee zero collision with any imported asset.
// -----------------------------------------------------------------------
namespace BuiltinMesh
{
	inline AssetID CubeID()
	{
		return AssetID::Create(0x0000010000000000LL, AssetType::StaticMesh);
	}
	inline AssetID CapsuleID()
	{
		return AssetID::Create(0x0000020000000000LL, AssetType::StaticMesh);
	}
}

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
// AssetRegistry is the authority for name/ID lookup. MeshManager holds
// only the GPU geometry slots and a minimal slot→AssetID reverse map.
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

	/// Register a MeshAsset — copies vertex/index data into the mega-buffers,
	/// then registers name/ID into AssetRegistry (Data = slot index).
	/// Returns the slot ID, or UINT32_MAX on failure.
	uint32_t RegisterMesh(const MeshAsset& asset, const std::string& name = {}, AssetID id = {});

	/// Register the built-in cube mesh as slot 0.
	uint32_t RegisterBuiltinCube();

	/// Register a built-in capsule mesh (two hemispheres + cylinder body).
	uint32_t RegisterBuiltinCapsule(float radius, float halfHeight, uint32_t segments);

	uint64_t GetVertexBufferAddr() const { return VertexMegaBuffer.DeviceAddr; }
	VkBuffer GetIndexBufferHandle() const { return static_cast<VkBuffer>(IndexMegaBuffer.Buffer); }
	uint64_t GetMeshTableAddr() const { return MeshTableBuffer.DeviceAddr; }
	const MeshSlot& GetSlot(uint32_t id) const { return Slots[id]; }
	uint32_t GetMeshCount() const { return MeshCount; }

	/// Find a mesh slot by display name. Returns UINT32_MAX if not registered.
	uint32_t FindSlotByName(const std::string& name) const
	{
		const AssetEntry* e = AssetRegistry::Get().FindByName(name);
		if (!e || e->Type != AssetType::StaticMesh) return UINT32_MAX;
		return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->Data));
	}

	/// Find a mesh slot by AssetID. Returns UINT32_MAX if not registered.
	uint32_t FindSlotByID(AssetID id) const
	{
		const AssetEntry* e = AssetRegistry::Get().Find(id);
		if (!e || e->Type != AssetType::StaticMesh) return UINT32_MAX;
		return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->Data));
	}

	/// Get the display name for a slot via AssetRegistry.
	const std::string& GetSlotName(uint32_t slot) const
	{
		static const std::string empty;
		if (slot >= MeshCount) return empty;
		const AssetEntry* e = AssetRegistry::Get().Find(SlotIDs[slot]);
		return e ? e->Name : empty;
	}

	AssetID GetSlotID(uint32_t slot) const { return SlotIDs[slot]; }

private:
	VulkanBuffer VertexMegaBuffer;
	VulkanBuffer IndexMegaBuffer;
	VulkanBuffer MeshTableBuffer; // GpuMeshInfo[MAX_MESH_SLOTS], PersistentMapped + BDA

	uint32_t NextVertexOffset = 0; // in vertices (not bytes)
	uint32_t NextIndexOffset  = 0; // in indices  (not bytes)
	uint32_t MeshCount        = 0;
	MeshSlot Slots[MAX_MESH_SLOTS]{};
	AssetID  SlotIDs[MAX_MESH_SLOTS]{}; // slot → AssetID reverse map for GetSlotName/GetSlotID
};
