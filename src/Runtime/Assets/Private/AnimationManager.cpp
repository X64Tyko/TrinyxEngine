#include "AnimationManager.h"
#include "AssetRegistry.h"
#include "AnimationAsset.h"
#include "Logger.h"
#include "TrinyxJobs.h"

#include <cstring>


bool AnimationManager::Initialize(VulkanMemory* vkMem)
{
	TrackBuffer = vkMem->AllocateBuffer(
		MAX_TOTAL_BONE_TRACKS * sizeof(GpuAnimBoneTrack),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		GpuMemoryDomain::PersistentMapped,
		/*requestDeviceAddress=*/true);

	if (!TrackBuffer.IsValid())
	{
		LOG_ENG_ERROR("[AnimationManager] Track buffer allocation failed");
		return false;
	}

	KeyframeBuffer = vkMem->AllocateBuffer(
		MAX_TOTAL_KEYFRAMES * sizeof(GpuAnimKeyframe),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		GpuMemoryDomain::PersistentMapped,
		/*requestDeviceAddress=*/true);

	if (!KeyframeBuffer.IsValid())
	{
		LOG_ENG_ERROR("[AnimationManager] Keyframe buffer allocation failed");
		return false;
	}

	std::memset(TrackBuffer.MappedPtr, 0, MAX_TOTAL_BONE_TRACKS * sizeof(GpuAnimBoneTrack));
	std::memset(KeyframeBuffer.MappedPtr, 0, MAX_TOTAL_KEYFRAMES * sizeof(GpuAnimKeyframe));

	AnimSlotBuffer = vkMem->AllocateBuffer(
		MAX_ANIM_SLOTS * sizeof(GpuAnimSlotInfo),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		GpuMemoryDomain::PersistentMapped,
		/*requestDeviceAddress=*/true);

	if (!AnimSlotBuffer.IsValid())
	{
		LOG_ENG_ERROR("[AnimationManager] Anim slot buffer allocation failed");
		return false;
	}

	std::memset(AnimSlotBuffer.MappedPtr, 0, MAX_ANIM_SLOTS * sizeof(GpuAnimSlotInfo));

	LOG_ENG_INFO_F("[AnimationManager] Initialized (tracks: %u @ %u KB, keyframes: %u @ %u MB)",
		MAX_TOTAL_BONE_TRACKS,
		static_cast<uint32_t>(MAX_TOTAL_BONE_TRACKS * sizeof(GpuAnimBoneTrack) / 1024),
		MAX_TOTAL_KEYFRAMES,
		static_cast<uint32_t>(MAX_TOTAL_KEYFRAMES * sizeof(GpuAnimKeyframe) / (1024 * 1024)));

	AssetRegistry::Get().RegisterUploadChecker(
		[](void*)
	{
		return AnimationManager::Get().IsUploadComplete();
	}, nullptr);
	return true;
}

void AnimationManager::Shutdown()
{
	TrackBuffer.Free();
	KeyframeBuffer.Free();
	AnimSlotBuffer.Free();
}

uint32_t AnimationManager::CommitToSlot(const AnimationAsset& asset, AssetID id)
{
	if (AnimCount >= MAX_ANIM_SLOTS)
	{
		LOG_ENG_ERROR("[AnimationManager] Animation slot limit reached");
		return UINT32_MAX;
	}

	const uint32_t keyframeCount = static_cast<uint32_t>(asset.keyframes.size());

	if (NextTrack + asset.boneCount > MAX_TOTAL_BONE_TRACKS)
	{
		LOG_ENG_ERROR("[AnimationManager] Track buffer overflow");
		return UINT32_MAX;
	}

	if (NextKeyframe + keyframeCount > MAX_TOTAL_KEYFRAMES)
	{
		LOG_ENG_ERROR("[AnimationManager] Keyframe buffer overflow");
		return UINT32_MAX;
	}

	uint32_t slotID = AnimCount++;
	SlotIDs[slotID] = id;

	if (id.IsValid()) AssetRegistry::Get().RegisterSlot(AssetType::Animation, slotID, id);

	Slots[slotID].trackOffset    = NextTrack;
	Slots[slotID].boneCount      = asset.boneCount;
	Slots[slotID].keyframeOffset = NextKeyframe;
	Slots[slotID].duration       = asset.duration;

	auto* gpuSlots               = static_cast<GpuAnimSlotInfo*>(AnimSlotBuffer.MappedPtr);
	gpuSlots[slotID].trackOffset = NextTrack;
	gpuSlots[slotID].boneCount   = asset.boneCount;

	CpuCopies[slotID] = asset; // full copy — vectors included

	if (id.IsValid())
	{
		if (AssetEntry* entry = AssetRegistry::Get().FindMutable(id))
			entry->CPUData = &CpuCopies[slotID];
	}

	// PersistentMapped — GPU sees writes before OnLoaded fires on the calling thread.
	GpuAnimBoneTrack* gpuTracks = static_cast<GpuAnimBoneTrack*>(TrackBuffer.MappedPtr) + NextTrack;
	GpuAnimKeyframe* gpuKfs     = static_cast<GpuAnimKeyframe*>(KeyframeBuffer.MappedPtr) + NextKeyframe;
	const uint32_t kfBase       = NextKeyframe;
	static_assert(sizeof(GpuAnimKeyframe) == sizeof(AnimKeyframe),
		"GpuAnimKeyframe and AnimKeyframe layout must match for bulk memcpy");

	for (uint32_t i = 0; i < asset.boneCount; ++i)
	{
		gpuTracks[i].keyframeOffset = kfBase + asset.boneTracks[i].keyframeOffset;
		gpuTracks[i].keyframeCount  = asset.boneTracks[i].keyframeCount;
	}
	std::memcpy(gpuKfs, asset.keyframes.data(),
		asset.keyframes.size() * sizeof(GpuAnimKeyframe));

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

	NextTrack += asset.boneCount;
	NextKeyframe += keyframeCount;

	LOG_ENG_INFO_F("[AnimationManager] Loaded anim slot %u (%.2fs, %u bones, %u keyframes)",
		slotID, asset.duration, asset.boneCount, keyframeCount);
	return slotID;
}

uint32_t AnimationManager::LoadAnimation(const AnimationAsset& asset, TnxName name, AssetID id)
{
	if (id.IsValid()) AssetRegistry::Get().Register(id, name, {}, AssetType::Animation);
	return CommitToSlot(asset, id);
}

uint32_t AnimationManager::LoadAnimation(AssetID id)
{
	uint32_t slot = FindSlotByID(id);
	if (slot != UINT32_MAX) return slot;

	const AssetEntry* entry = AssetRegistry::Get().Find(id);
	if (!entry || entry->Type != AssetType::Animation)
	{
		LOG_ENG_ERROR("[AnimationManager] LoadAnimation: AssetID not in registry");
		return UINT32_MAX;
	}

	std::string path = AssetRegistry::Get().ResolvePath(id);
	if (path.empty())
	{
		LOG_ENG_ERROR("[AnimationManager] LoadAnimation: no resolvable path for AssetID");
		return UINT32_MAX;
	}

	AnimationAsset asset;
	if (!LoadAnimationAsset(asset, path))
	{
		LOG_ENG_ERROR_F("[AnimationManager] LoadAnimation: failed to decode '%s'", path.c_str());
		return UINT32_MAX;
	}

	return CommitToSlot(asset, id);
}

uint32_t AnimationManager::LoadAnimation(TnxName name)
{
	const AssetEntry* entry = AssetRegistry::Get().FindByTNameAndType(name, AssetType::Animation);
	if (!entry)
	{
		LOG_ENG_ERROR_F("[AnimationManager] LoadAnimation: TnxName '%s' not in registry",
			name.GetStr());
		return UINT32_MAX;
	}
	return LoadAnimation(entry->ID);
}

void AnimationManager::FlushPendingUploads()
{
	TrinyxJobs::WaitForCounter(&GpuUploadCounter, TrinyxJobs::Queue::Render);
}

const AnimationAsset* AnimationManager::GetAnimCPU(uint32_t slot) const
{
	if (slot == 0 || slot >= AnimCount) return nullptr;
	return &CpuCopies[slot];
}
