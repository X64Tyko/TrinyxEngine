#pragma once
#include <cstdint>
#include <string>

#include "AssetRegistry.h"
#include "AssetTypes.h"
#include "SkeletonAsset.h"
#include "TrinyxJobs.h"
#include "VulkanMemory.h"

// -----------------------------------------------------------------------
// GpuBoneData — per-bone inverse bind pose uploaded to GPU.
// One entry per bone across all loaded skeletons (indexed by global bone
// offset, not per-skeleton index).
// -----------------------------------------------------------------------

struct GpuBoneData
{
	float inverseBindPose[16]; // column-major mat4
};

static_assert(sizeof(GpuBoneData) == 64, "GpuBoneData must be 64 bytes");

// -----------------------------------------------------------------------
// GpuSkeletonSlotInfo — per-skeleton slot table entry.
// skinning.slang reads this to locate a skeleton's bones in the global
// bone mega-buffer. Indexed by skelID (slot index in SkeletonManager).
// -----------------------------------------------------------------------

struct GpuSkeletonSlotInfo
{
	uint32_t boneOffset = 0; // first GpuBoneData entry for this skeleton
	uint32_t boneCount  = 0;
};

static_assert(sizeof(GpuSkeletonSlotInfo) == 8, "GpuSkeletonSlotInfo must be 8 bytes");

// -----------------------------------------------------------------------
// SkeletonManager — mega-buffer management for all skeleton bone data.
//
// All loaded skeletons sub-allocate into a single GpuBoneData mega-buffer.
// CPU copies of SkeletonAsset are retained for hierarchy chain-walking
// during socket evaluation in ESkeletalEntity::GetSocketTransform.
//
// AssetRegistry is the authority for name/ID lookup.
// Slot 0 is reserved as the invalid/error sentinel.
// -----------------------------------------------------------------------

class SkeletonManager
{
public:
	static constexpr uint32_t MAX_SKELETON_SLOTS = 256;
	static constexpr uint32_t MAX_TOTAL_BONES    = 65536;

	struct SkeletonSlot
	{
		uint32_t boneOffset = 0; // first GpuBoneData entry for this skeleton
		uint32_t boneCount  = 0;
	};

	static SkeletonManager& Get()
	{
		static SkeletonManager instance;
		return instance;
	}

	bool Initialize(VulkanMemory* vkMem);

	/// Free GPU buffers. Must be called before VulkanMemory::Shutdown().
	void Shutdown();

	/// Load a SkeletonAsset — copies bone data into the mega-buffer and retains
	/// a CPU copy for chain walking. Records name/ID in AssetRegistry.
	/// Returns the slot ID, or UINT32_MAX on failure.
	uint32_t LoadSkeleton(const SkeletonAsset& asset, TnxName name, AssetID id = {});

	/// Resolve by AssetID from AssetRegistry, decode from disk, and load.
	uint32_t LoadSkeleton(AssetID id);

	/// Resolve by TnxName from AssetRegistry, decode from disk, and load.
	uint32_t LoadSkeleton(TnxName name);

	/// CPU skeleton for hierarchy queries (chain walking, socket lookup).
	const SkeletonAsset* GetSkeletonCPU(uint32_t slot) const;

	uint64_t GetBoneDataAddr() const { return BoneDataBuffer.DeviceAddr; }
	uint64_t GetBoneParentAddr() const { return BoneParentBuffer.DeviceAddr; }
	uint64_t GetSkeletonSlotAddr() const { return SkeletonSlotBuffer.DeviceAddr; }
	const SkeletonSlot& GetSlot(uint32_t slot) const { return Slots[slot]; }
	uint32_t GetSkeletonCount() const { return SkeletonCount; }

	/// Block until all pending GPU bone-data upload jobs have completed.
	void FlushPendingUploads();

	/// Non-blocking poll — true when all GPU upload jobs have completed.
	bool IsUploadComplete() const { return GpuUploadCounter.Value.load(std::memory_order_acquire) == 0; }

	uint32_t FindSlotByTName(TnxName name) const
	{
		const AssetEntry* e = AssetRegistry::Get().FindByTNameAndType(name, AssetType::Skeleton);
		if (!e || !e->Data) return UINT32_MAX;
		return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->Data));
	}

	uint32_t FindSlotByID(AssetID id) const
	{
		const AssetEntry* e = AssetRegistry::Get().Find(id);
		if (!e || e->Type != AssetType::Skeleton || !e->Data) return UINT32_MAX;
		return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->Data));
	}

	AssetID GetSlotID(uint32_t slot) const { return SlotIDs[slot]; }

private:
	/// Copy asset data into mega-buffer and CPU copy array, update AssetRegistry.
	/// Does NOT call Register() — caller is responsible for registration.
	uint32_t CommitToSlot(const SkeletonAsset& asset, AssetID id);

	VulkanBuffer BoneDataBuffer;     // GpuBoneData[MAX_TOTAL_BONES], PersistentMapped + BDA
	VulkanBuffer BoneParentBuffer;   // uint32[MAX_TOTAL_BONES] parent indices, PersistentMapped + BDA
	VulkanBuffer SkeletonSlotBuffer; // GpuSkeletonSlotInfo[MAX_SKELETON_SLOTS], PersistentMapped + BDA

	GpuBoneData BoneData[MAX_TOTAL_BONES]{};
	SkeletonSlot Slots[MAX_SKELETON_SLOTS]{};
	AssetID SlotIDs[MAX_SKELETON_SLOTS]{};
	SkeletonAsset CpuCopies[MAX_SKELETON_SLOTS];
	TrinyxJobs::JobCounter GpuUploadCounter;
	uint32_t NextBoneOffset = 0;
	uint32_t SkeletonCount  = 1; // slot 0 reserved as invalid sentinel
};
