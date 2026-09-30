#include "SkeletonManager.h"
#include "AssetRegistry.h"
#include "SkeletonAsset.h"
#include "Logger.h"
#include "TrinyxJobs.h"

#include <cstring>


// -----------------------------------------------------------------------
// Initialize
// -----------------------------------------------------------------------

bool SkeletonManager::Initialize(VulkanMemory* vkMem)
{
	BoneDataBuffer = vkMem->AllocateBuffer(
		MAX_TOTAL_BONES * sizeof(GpuBoneData),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		GpuMemoryDomain::PersistentMapped,
		/*requestDeviceAddress=*/true);

	if (!BoneDataBuffer.IsValid())
	{
		LOG_ENG_ERROR("[SkeletonManager] Bone data buffer allocation failed");
		return false;
	}

	std::memset(BoneDataBuffer.MappedPtr, 0, MAX_TOTAL_BONES * sizeof(GpuBoneData));

	BoneParentBuffer = vkMem->AllocateBuffer(
		MAX_TOTAL_BONES * sizeof(uint32_t),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		GpuMemoryDomain::PersistentMapped,
		/*requestDeviceAddress=*/true);

	if (!BoneParentBuffer.IsValid())
	{
		LOG_ENG_ERROR("[SkeletonManager] Bone parent buffer allocation failed");
		return false;
	}

	// 0xFFFFFFFF = root (no parent) — memset 0xFF initializes all entries to UINT32_MAX.
	std::memset(BoneParentBuffer.MappedPtr, 0xFF, MAX_TOTAL_BONES * sizeof(uint32_t));

	SkeletonSlotBuffer = vkMem->AllocateBuffer(
		MAX_SKELETON_SLOTS * sizeof(GpuSkeletonSlotInfo),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		GpuMemoryDomain::PersistentMapped,
		/*requestDeviceAddress=*/true);

	if (!SkeletonSlotBuffer.IsValid())
	{
		LOG_ENG_ERROR("[SkeletonManager] Skeleton slot buffer allocation failed");
		return false;
	}

	std::memset(SkeletonSlotBuffer.MappedPtr, 0, MAX_SKELETON_SLOTS * sizeof(GpuSkeletonSlotInfo));

	LOG_ENG_INFO_F("[SkeletonManager] Initialized (max bones: %u, buffer: %.1f MB)",
		MAX_TOTAL_BONES,
		static_cast<float>(MAX_TOTAL_BONES * sizeof(GpuBoneData)) / (1024.f * 1024.f));

	AssetRegistry::Get().RegisterUploadChecker(
		[](void*)
	{
		return SkeletonManager::Get().IsUploadComplete();
	}, nullptr);
	return true;
}

// -----------------------------------------------------------------------
// Shutdown
// -----------------------------------------------------------------------

void SkeletonManager::Shutdown()
{
	BoneDataBuffer.Free();
	BoneParentBuffer.Free();
	SkeletonSlotBuffer.Free();
}

// -----------------------------------------------------------------------
// CommitToSlot — internal; copies bone data into mega-buffer and CPU copy.
// Does NOT call Register() — caller owns that.
// -----------------------------------------------------------------------

uint32_t SkeletonManager::CommitToSlot(const SkeletonAsset& asset, AssetID id)
{
	if (SkeletonCount >= MAX_SKELETON_SLOTS)
	{
		LOG_ENG_ERROR("[SkeletonManager] Skeleton slot limit reached");
		return UINT32_MAX;
	}

	if (NextBoneOffset + asset.boneCount > MAX_TOTAL_BONES)
	{
		LOG_ENG_ERROR("[SkeletonManager] Bone mega-buffer overflow");
		return UINT32_MAX;
	}

	uint32_t slotID = SkeletonCount++;
	SlotIDs[slotID] = id;

	if (id.IsValid()) AssetRegistry::Get().RegisterSlot(AssetType::Skeleton, slotID, id);

	Slots[slotID].boneOffset = NextBoneOffset;
	Slots[slotID].boneCount  = asset.boneCount;

	auto* gpuSlots              = static_cast<GpuSkeletonSlotInfo*>(SkeletonSlotBuffer.MappedPtr);
	gpuSlots[slotID].boneOffset = NextBoneOffset;
	gpuSlots[slotID].boneCount  = asset.boneCount;

	// Fill CPU shadow (needed synchronously for chain walks / socket queries).
	for (uint32_t i = 0; i < asset.boneCount; ++i)
		std::memcpy(BoneData[NextBoneOffset + i].inverseBindPose,
			asset.bones[i].inverseBindPose, sizeof(float) * 16);

	CpuCopies[slotID] = asset; // full copy — bones + sockets retained for chain walks

	if (id.IsValid())
	{
		if (AssetEntry* entry = AssetRegistry::Get().FindMutable(id))
			entry->CPUData = &CpuCopies[slotID];
	}

	// Fill GPU bone buffers synchronously — PersistentMapped, so writes are immediately
	// visible to the GPU. Mirrors MeshManager's synchronous approach so that OnLoaded
	// fires on the calling (Logic) thread with valid FieldProxy cursors.
	GpuBoneData* gpuBones = static_cast<GpuBoneData*>(BoneDataBuffer.MappedPtr) + NextBoneOffset;
	uint32_t* gpuParents  = static_cast<uint32_t*>(BoneParentBuffer.MappedPtr) + NextBoneOffset;
	for (uint32_t i = 0; i < asset.boneCount; ++i)
	{
		std::memcpy(gpuBones[i].inverseBindPose, asset.bones[i].inverseBindPose, sizeof(float) * 16);
		gpuParents[i] = asset.bones[i].parentIndex;
	}

	if (id.IsValid())
	{
		if (AssetEntry* entry = AssetRegistry::Get().FindMutable(id))
		{
			entry->Data  = reinterpret_cast<void*>(static_cast<uintptr_t>(slotID));
			entry->State = RuntimeFlags::Loaded;
			entry->OnLoaded(slotID);
			entry->OnLoaded.Reset();
		}
	}

	NextBoneOffset += asset.boneCount;

	LOG_ENG_INFO_F("[SkeletonManager] Loaded skeleton slot %u (%u bones, %u sockets)",
		slotID, asset.boneCount, asset.socketCount);
	return slotID;
}

// -----------------------------------------------------------------------
// LoadSkeleton
// -----------------------------------------------------------------------

uint32_t SkeletonManager::LoadSkeleton(const SkeletonAsset& asset, TnxName name, AssetID id)
{
	if (id.IsValid()) AssetRegistry::Get().Register(id, name, {}, AssetType::Skeleton);
	return CommitToSlot(asset, id);
}

uint32_t SkeletonManager::LoadSkeleton(AssetID id)
{
	uint32_t slot = FindSlotByID(id);
	if (slot != UINT32_MAX) return slot;

	const AssetEntry* entry = AssetRegistry::Get().Find(id);
	if (!entry || entry->Type != AssetType::Skeleton)
	{
		LOG_ENG_ERROR("[SkeletonManager] LoadSkeleton: AssetID not in registry");
		return UINT32_MAX;
	}

	std::string path = AssetRegistry::Get().ResolvePath(id);
	if (path.empty())
	{
		LOG_ENG_ERROR("[SkeletonManager] LoadSkeleton: no resolvable path for AssetID");
		return UINT32_MAX;
	}

	SkeletonAsset asset;
	if (!LoadSkeletonAsset(asset, path))
	{
		LOG_ENG_ERROR_F("[SkeletonManager] LoadSkeleton: failed to decode '%s'", path.c_str());
		return UINT32_MAX;
	}

	return CommitToSlot(asset, id);
}

uint32_t SkeletonManager::LoadSkeleton(TnxName name)
{
	const AssetEntry* entry = AssetRegistry::Get().FindByTNameAndType(name, AssetType::Skeleton);
	if (!entry)
	{
		LOG_ENG_ERROR_F("[SkeletonManager] LoadSkeleton: TnxName '%s' not in registry",
			name.GetStr());
		return UINT32_MAX;
	}
	return LoadSkeleton(entry->ID);
}

// -----------------------------------------------------------------------
// FlushPendingUploads
// -----------------------------------------------------------------------

void SkeletonManager::FlushPendingUploads()
{
	TrinyxJobs::WaitForCounter(&GpuUploadCounter, TrinyxJobs::Queue::Render);
}

// -----------------------------------------------------------------------
// GetSkeletonCPU
// -----------------------------------------------------------------------

const SkeletonAsset* SkeletonManager::GetSkeletonCPU(uint32_t slot) const
{
	if (slot == 0 || slot >= SkeletonCount) return nullptr;
	return &CpuCopies[slot];
}
