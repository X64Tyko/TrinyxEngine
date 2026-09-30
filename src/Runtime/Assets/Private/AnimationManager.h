#pragma once
#include <cstdint>
#include <string>

#include "AssetRegistry.h"
#include "AssetTypes.h"
#include "AnimationAsset.h"
#include "TrinyxJobs.h"
#include "VulkanMemory.h"

/// Per-animation slot read by skinning.slang to locate bone tracks in the global mega-buffer.
struct GpuAnimSlotInfo
{
	uint32_t trackOffset = 0; // first GpuAnimBoneTrack entry for this animation
	uint32_t boneCount   = 0;
};

static_assert(sizeof(GpuAnimSlotInfo) == 8, "GpuAnimSlotInfo must be 8 bytes");

/// Per-bone keyframe range within the global GpuAnimKeyframe mega-buffer.
/// keyframeOffset is an absolute index; the skinning shader indexes directly.
struct GpuAnimBoneTrack
{
	uint32_t keyframeOffset; ///< Absolute index into the global GpuAnimKeyframe array.
	uint32_t keyframeCount;
};

static_assert(sizeof(GpuAnimBoneTrack) == 8, "GpuAnimBoneTrack must be 8 bytes");

/// Single keyframe in the global mega-buffer: translation + rotation quaternion (no scale in M1).
struct GpuAnimKeyframe
{
	float time;
	float tx, ty, tz;     ///< Translation.
	float rx, ry, rz, rw; ///< Rotation quaternion (normalized).
};

static_assert(sizeof(GpuAnimKeyframe) == 32, "GpuAnimKeyframe must be 32 bytes");

/// Manages GPU mega-buffers for animation tracks, keyframes, and slot tables.
/// Slot 0 is reserved as the invalid sentinel; valid slot IDs start at 1.
class AnimationManager
{
public:
	static constexpr uint32_t MAX_ANIM_SLOTS        = 2048;
	static constexpr uint32_t MAX_TOTAL_BONE_TRACKS = 131072;  // avg 64 bones × 2048 anims
	static constexpr uint32_t MAX_TOTAL_KEYFRAMES   = 4194304; // ~4M keyframes

	struct AnimSlot
	{
		uint32_t trackOffset; // first GpuAnimBoneTrack in TrackBuffer for this anim
		uint32_t boneCount;
		uint32_t keyframeOffset; // first GpuAnimKeyframe in KeyframeBuffer for this anim
		float duration;
	};

	static AnimationManager& Get()
	{
		static AnimationManager instance;
		return instance;
	}

	/// Allocate GPU mega-buffers (PersistentMapped + BDA). Must be called before any LoadAnimation.
	bool Initialize(VulkanMemory* vkMem);

	/// Must be called before VulkanMemory::Shutdown().
	void Shutdown();

	/// Returns the slot ID, or UINT32_MAX on failure.
	uint32_t LoadAnimation(const AnimationAsset& asset, TnxName name, AssetID id = {});
	uint32_t LoadAnimation(AssetID id);
	uint32_t LoadAnimation(TnxName name);

	/// Returns the CPU-side AnimationAsset for @p slot, or nullptr if the slot is invalid.
	const AnimationAsset* GetAnimCPU(uint32_t slot) const;

	float GetDuration(uint32_t slot) const
	{
		return (slot > 0 && slot < AnimCount) ? Slots[slot].duration : 0.f;
	}

	uint64_t GetTrackBufferAddr() const { return TrackBuffer.DeviceAddr; }
	uint64_t GetKeyframeBufferAddr() const { return KeyframeBuffer.DeviceAddr; }
	uint64_t GetAnimSlotAddr() const { return AnimSlotBuffer.DeviceAddr; }
	const AnimSlot& GetSlot(uint32_t slot) const { return Slots[slot]; }
	uint32_t GetAnimCount() const { return AnimCount; }

	/// Submit all pending PersistentMapped writes to the GPU; called once per render frame.
	void FlushPendingUploads();
	/// Returns true when all in-flight GPU upload jobs have completed.
	bool IsUploadComplete() const { return GpuUploadCounter.Value.load(std::memory_order_acquire) == 0; }

	/// Resolve an animation slot from a TnxName; returns UINT32_MAX if not loaded.
	uint32_t FindSlotByTName(TnxName name) const
	{
		const AssetEntry* e = AssetRegistry::Get().FindByTNameAndType(name, AssetType::Animation);
		if (!e || !e->Data) return UINT32_MAX;
		return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->Data));
	}

	/// Resolve an animation slot from an AssetID; returns UINT32_MAX if not loaded.
	uint32_t FindSlotByID(AssetID id) const
	{
		const AssetEntry* e = AssetRegistry::Get().Find(id);
		if (!e || e->Type != AssetType::Animation || !e->Data) return UINT32_MAX;
		return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->Data));
	}

	AssetID GetSlotID(uint32_t slot) const { return SlotIDs[slot]; }

private:
	/// Does NOT call Register() — caller owns that.
	uint32_t CommitToSlot(const AnimationAsset& asset, AssetID id);

	VulkanBuffer TrackBuffer;    // GpuAnimBoneTrack[MAX_TOTAL_BONE_TRACKS], PersistentMapped + BDA
	VulkanBuffer KeyframeBuffer; // GpuAnimKeyframe[MAX_TOTAL_KEYFRAMES], PersistentMapped + BDA
	VulkanBuffer AnimSlotBuffer; // GpuAnimSlotInfo[MAX_ANIM_SLOTS], PersistentMapped + BDA

	AnimSlot Slots[MAX_ANIM_SLOTS]{};
	AssetID SlotIDs[MAX_ANIM_SLOTS]{};
	AnimationAsset CpuCopies[MAX_ANIM_SLOTS];
	TrinyxJobs::JobCounter GpuUploadCounter;
	uint32_t NextTrack    = 0;
	uint32_t NextKeyframe = 0;
	uint32_t AnimCount    = 1; // slot 0 reserved as invalid sentinel
};
