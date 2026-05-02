#pragma once
#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>
#include <unordered_map>

#include "Types.h"
#include "EngineConfig.h"
#include "Logger.h"
#include "QuatMath.h"

namespace TrinyxJobs
{
	struct JobCounter;
}

enum class SystemID : uint8_t;
class Archetype;

// Function pointer types for tier-specific behavior
using FnGetNextWriteFramePtr = uint32_t (*)(const class ComponentCacheBase*);
using FnPropagateFramePtr = void (*)(class ComponentCacheBase*, TrinyxJobs::JobCounter&);

struct alignas(64) TemporalFrameHeader
{
	// Ownership tracking (atomic bitfield)
	std::atomic<uint8_t> OwnershipFlags;
	/*
        0x01 = LOGIC_WRITING
        0x02 = RENDER_READING
        0x04 = NETWORK_READING
        0x08 = DEFRAG_LOCKED
        Multiple readers can coexist (bitwise OR)
    */
	uint8_t _pad;

	// Frame identification
	uint32_t FrameNumber;

	// Camera/View data
	Vector3 CameraPosition;
	Vector3 PrevCameraPosition;
	Quat CameraRotation;
	Quat PrevCameraRotation;
	SimFloat CameraFoV;
	SimFloat PrevCameraFoV;

	// Scene/Lighting data
	Vector3 SunDirection;
	Vector3 SunColor;
	SimFloat AmbientIntensity;

#if TNX_DEV_METRICS
	// Input-to-photon latency tracking
	uint64_t InputTimestamp; // perf counter when last input event arrived
#endif

	// Entity metadata
	uint32_t ActiveEntityCount;
	uint32_t TotalAllocatedEntities;

#ifdef TNX_ENABLE_ROLLBACK
	// Input snapshot for deterministic replay during rollback.
	// Recorded after ProcessSimInput each tick; replayed during resimulation.
	uint8_t InputKeyState[64];
	SimFloat InputMouseDX;
	SimFloat InputMouseDY;
#endif

	// Padding to cache line
#if defined(TNX_ENABLE_ROLLBACK) && TNX_DEV_METRICS
	char _padding[64 - (sizeof(std::atomic<uint8_t>) + sizeof(uint8_t) + sizeof(uint64_t) + sizeof(uint32_t) * 3
		+ sizeof(Vector3) * 3 + sizeof(SimFloat) + 64 + sizeof(SimFloat) * 2) % 64];
#elif defined(TNX_ENABLE_ROLLBACK)
	char _padding[64 - (sizeof(std::atomic<uint8_t>) + sizeof(uint8_t) + sizeof(uint32_t) * 3
		+ sizeof(Vector3) * 3 + sizeof(SimFloat) + 64 + sizeof(SimFloat) * 2) % 64];
#elif TNX_DEV_METRICS
	char _padding[64 - (sizeof(std::atomic<uint8_t>) + sizeof(uint8_t) + sizeof(uint64_t) + sizeof(uint32_t) * 3 + sizeof(Vector3) * 3 + sizeof(SimFloat)) % 64];
#else
	char _padding[64 - (sizeof(std::atomic<uint8_t>) + sizeof(uint8_t) + sizeof(uint32_t) * 3 + sizeof(Vector3) * 3 + sizeof(SimFloat)) % 64];
#endif
};

// ─────────────────────────────────────────────────────────────────────────────
// Non-template concrete base — all data and all method implementations.
// Archetype, LogicThread, and RenderThread store ComponentCacheBase*.
// No virtual functions: Initialize is not on this class; all other methods are
// concrete and shared between Volatile and Temporal tiers.
// ─────────────────────────────────────────────────────────────────────────────
class ComponentCacheBase
{
public:
	ComponentCacheBase();
	~ComponentCacheBase();

	// O(1) frame header access - external systems manage locking
	TemporalFrameHeader* GetFrameHeader(int32_t frameNum = -1) const
	{
		frameNum = frameNum == -1 ? ActiveWriteFrame : frameNum;
		return FrameHeaders[frameNum % TemporalFrameCount];
	}

	// Frame locking for multi-threaded access
	// We assume that after propagating the next frame is locked for writing until propagated again.
	// outWriteFrame is the actual frame we're writing to, for field data collection
	bool TryLockFrameForWrite(uint32_t& outWriteFrame);
	bool VerifyFrameReadable(uint32_t bufferIndex) const;
	void UnlockFrameWrite();
	bool TryLockFrameForRead(uint32_t frameNum);
	void UnlockFrameRead(uint32_t frameNum);

	uint32_t GetActiveWriteFrame() const { return ActiveWriteFrame; }
	uint32_t GetActiveReadFrame() const { return LastWrittenFrame; }

	// Allocate field array for a chunk across all frames, returns absolute pointer to frame 0 data.
	// Archetype calls this for each temporal field when allocating a new chunk.
	void* AllocateFieldArray(Archetype* owner, struct Chunk* chunk, CacheSlotID cacheSlot,
							 size_t fieldIndex, const char* fieldName, size_t entityCount, size_t fieldSize, SystemID EntitySystemID);

	// Since we're allocating one field at a time and we need them in line we have to advance the allocator manually for now.
	size_t AdvanceAllocator(SystemID EntitySystemID, size_t entityCount, size_t fieldSize);

	void ResetAllocators();
	void ClearFrameData();

	// Get the stride between frames (for calculating frame N from frame 0 pointer)
	FORCE_INLINE size_t GetFrameStride() const { return sizeof(TemporalFrameHeader) + FrameDataCapacity; }

	// Advance a frame-0 base pointer to the current write or read frame.
	// Use these instead of manually computing base + frame * stride.
	FORCE_INLINE void* GetWriteFramePtr(void* frame0Base) const
	{
		return static_cast<uint8_t*>(frame0Base) + ActiveWriteFrame * GetFrameStride();
	}
	FORCE_INLINE void* GetReadFramePtr(void* frame0Base) const
	{
		return static_cast<uint8_t*>(frame0Base) + LastWrittenFrame * GetFrameStride();
	}

	// Copy field data from fromFrame into toFrame before dispatch.
	// Called once per logic tick so all FieldProxy writes start from the previous frame's state.
	void PropagateFrameData(uint32_t fromFrame, uint32_t toFrame, TrinyxJobs::JobCounter& counter);

	// Get component field data from specific frame.
	// Also returns how many entities are allocated/valid for this field so render can clamp scans safely.
	void* GetFieldData(TemporalFrameHeader* header, CacheSlotID cacheSlot, size_t fieldIndex) const;

	uint32_t GetTotalFrameCount() const { return static_cast<uint32_t>(TemporalFrameCount); }
	CacheTier GetTier() const { return Tier_; }
	uint32_t GetMaxCachedEntityCount() const { return static_cast<uint32_t>(MaxCachedBoundary / sizeof(SimFloat)); }

	// Returns [start, end) cache index range for the contiguous DUAL+PHYS partition.
	// Physics systems iterate this as a single dense scan with no gap.
	std::pair<uint32_t, uint32_t> GetPhysicsRange() const
	{
		return {
			static_cast<uint32_t>((MaxRenderableBoundary - DualOffset) / 4),
			static_cast<uint32_t>((MaxRenderableBoundary + PhysOffset) / 4)
		};
	}

	// Returns a reference to the allocator offset for the given partition.
	// Render grows right from 0 (Arena 1); Dual grows left from MaxRenderable (Arena 1);
	// Phys grows right from MaxRenderable (Arena 2); Logic grows left from MaxCached (Arena 2).
	size_t GetSystemAllocatorIndex(SystemID sysID, size_t size) const;

	size_t AdvanceSystemAllocatorIndex(SystemID sysID, size_t size);
	
	uint32_t GetNextWriteFrame() const
	{
		return FnGetNextWriteFrame(this);
	}

	void PropagateFrame(TrinyxJobs::JobCounter& counter)
	{
		FnPropagateFrame(this, counter);
	}

#ifdef TNX_ENABLE_ROLLBACK
	// Rollback test support — direct manipulation of frame pointers and slab access.
	void SetActiveWriteFrame(uint32_t frame) { ActiveWriteFrame = frame; }
	void SetLastWrittenFrame(uint32_t frame) { LastWrittenFrame = frame; }
	void* GetSlabPtr() const { return SlabPtr; }
	size_t GetTotalSlabSize() const { return TotalSlabSize; }
	size_t GetFrameDataCapacity() const { return FrameDataCapacity; }

	struct FieldCompareInfo
	{
		ComponentTypeID CompType;
		size_t FieldIndex;
		const char* FieldName;
		size_t OffsetInFrame;
		size_t CurrentUsed;
		size_t FieldSize;
	};

	std::vector<FieldCompareInfo> GetValidFieldInfos() const;
#endif

protected:
	template <typename Derived>
	friend class ComponentCacheImpl;
	
	template <CacheTier Tier>
	friend class ComponentCache;
	
	// Called by ComponentCacheImpl::Initialize with the correct frame count for the tier.
	void InitializeInternal(const EngineConfig* Config, uint32_t frameCount);

	// Internal Lock, still waits for it to be available, but allows us to lock any frame
	bool LockFrameForWrite(uint32_t WriteFrame);

	FnGetNextWriteFramePtr FnGetNextWriteFrame = nullptr;
	FnPropagateFramePtr FnPropagateFrame = nullptr;
	
	// Set by ComponentCacheImpl before calling InitializeInternal so GetTier() is valid immediately.
	CacheTier Tier_           = CacheTier::Volatile;
	uint32_t LastWrittenFrame = 0;
	uint32_t ActiveWriteFrame = 0;

	size_t MaxRenderableBoundary = 0;
	size_t MaxCachedBoundary     = 0;

	// Per-partition allocation cursors (entity counts, not bytes).
	// Converted to byte offsets at allocation time by multiplying by fieldSize.
	size_t RenderOffset = 0; // Arena 1, grows right from 0
	size_t DualOffset   = 0; // Arena 1, grows left  from MaxRenderable
	size_t PhysOffset   = 0; // Arena 2, grows right from MaxRenderable
	size_t LogicOffset  = 0; // Arena 2, grows left  from MaxCached

private:
	// Pre-computed field allocation zones (field-major ordering across all archetypes)
	struct FieldAllocationInfo
	{
		ComponentTypeID CompType = 0; // Component type ID (for diagnostics); table is indexed by CacheSlotID
		size_t FieldIndex        = 0;
		const char* FieldName    = nullptr;

		size_t OffsetInFrame = 0; // Start of this field's allocation zone in each frame
		size_t TotalCapacity = 0; // Max size this field can allocate (sum across all archetypes)
		size_t CurrentUsed   = 0; // How much is currently allocated (bytes)

		size_t FieldSize = 0; // Size of each element for this field (bytes)
		bool bValid      = false;
	};

	static constexpr size_t FIELD_ALLOCATION_COUNT = MAX_COMPONENTS * MAX_TEMPORAL_FIELDS_PER_COMPONENT;

	// Flat table: index = compType * MAX_TEMPORAL_FIELDS_PER_COMPONENT + fieldIndex
	FieldAllocationInfo FieldAllocations[FIELD_ALLOCATION_COUNT]{};

	// Compact list of valid FieldAllocationInfo entries for fast iteration.
	// Populated in InitializeInternal; avoids scanning all 16k table slots.
	std::vector<uint16_t> ValidFields;

	// Active allocations with back-refs to chunks for defrag
	struct TemporalAllocation
	{
		Archetype* Owner;
		struct Chunk* OwnerChunk;
		size_t FieldAllocationIndex; // Which FieldAllocationInfo this belongs to
		size_t OffsetInFieldZone;    // Offset within that field's zone
		size_t Size;                 // entityCount * fieldSize (aligned)
	};

	std::vector<TemporalAllocation> ActiveAllocations;

	// One large slab storing multiple frames of history
	void* SlabPtr             = nullptr;
	size_t FrameDataCapacity  = 0; // Max size per frame (all field allocations)
	size_t TemporalFrameCount = 0; // Number of frames stored in this slab
	size_t TotalSlabSize      = 0; // Total allocation size

	// O(1) frame access array
	std::vector<TemporalFrameHeader*> FrameHeaders;

	static size_t AlignSize(size_t size);
};

// ─────────────────────────────────────────────────────────────────────────────
// CRTP middle layer.  Routes Initialize() to the derived class for:
//   GetFrameCount(Config)  — how many history frames to allocate
//   GetCacheTier()         — which CacheTier enum value this instance is
//   GetLabel()             — human-readable name for logging
// Everything else (all data, all frame/lock/alloc methods) lives in the base.
// ─────────────────────────────────────────────────────────────────────────────
template <typename Derived>
class ComponentCacheImpl : public ComponentCacheBase
{
public:
	void Initialize(const EngineConfig* Config)
	{
		Tier_ = static_cast<Derived*>(this)->GetCacheTier();
		FnGetNextWriteFrame = &Derived::GetNextWriteFrameImpl;
		FnPropagateFrame = &Derived::PropagateFrameImpl;
		InitializeInternal(Config, static_cast<Derived*>(this)->GetFrameCount(Config));
	}
	
protected:
	friend Derived;
};

// ─────────────────────────────────────────────────────────────────────────────
// Concrete cache type.  Registry instantiates these; Archetype/LogicThread/
// RenderThread use the ComponentCacheBase* pointer.
//
// The 3 CRTP overrides that differ between tiers:
//   GetFrameCount  — Volatile: 3 (fixed, triple-buffer), Temporal: Config->TemporalFrameCount
//   GetCacheTier   — returns the CacheTier enum value
//   GetLabel       — "Volatile" or "Temporal" for logging
// ─────────────────────────────────────────────────────────────────────────────
template <CacheTier Tier>
class ComponentCache final : public ComponentCacheImpl<ComponentCache<Tier>>
{
public:
	uint32_t GetFrameCount(const EngineConfig* Config) const
	{
		if constexpr (Tier == CacheTier::Volatile)
		{
			(void)Config;
			return 3;
		}
		else
		{
#ifndef TNX_ENABLE_ROLLBACK
			return 3;
#endif
			return static_cast<uint32_t>(Config->TemporalFrameCount);
		}
	}

	static constexpr CacheTier GetCacheTier() { return Tier; }

	static constexpr const char* GetLabel()
	{
		if constexpr (Tier == CacheTier::Volatile) return "Volatile";
		if constexpr (Tier == CacheTier::Universal) return "Universal";
		else return "Temporal";
	}

	// Static implementations for function pointers
	static uint32_t GetNextWriteFrameImpl(const ComponentCacheBase* base)
	{
		if constexpr (Tier == CacheTier::Volatile)
		{
			// Triple-buffer: find the frame that's not locked
			// We have 3 frames: one being written (T+1), one being read (T), one free (T-1)
			// Return the buffer slot index (0-2) of the unlocked frame
			// It's possible another thread is using the buffer for networking or something, loop until we find a valid frame
			while (true)
			{
				for (uint32_t i = 0; i < 3; ++i)
				{
					uint32_t candidate = (base->ActiveWriteFrame + 1 + i) % base->GetTotalFrameCount();
					if (candidate == base->ActiveWriteFrame) continue;
					TemporalFrameHeader* header = base->GetFrameHeader(candidate);
					uint8_t flags = header->OwnershipFlags.load(std::memory_order_acquire);
					// Frame is free if not write-locked and not read-locked
					if ((flags & 0x03) == 0) // 0x01 = LOGIC_WRITING, 0x02 = RENDER_READING
					{
						return candidate;
					}
				}
			}
		}
		else
		{
			// Temporal/Universal: circular buffer, just increment with modulo
			return (base->ActiveWriteFrame + 1) % base->GetTotalFrameCount();
		}
	}

	static void PropagateFrameImpl(ComponentCacheBase* base, TrinyxJobs::JobCounter& counter)
	{
		uint32_t targetSlot = (base->ActiveWriteFrame + 1) % base->GetTotalFrameCount();
		if constexpr (Tier == CacheTier::Volatile)
		{
			// Triple-buffer: find the frame that's not locked
			// We have 3 frames: one being written (T+1), one being read (T), one free (T-1)
			// Return the buffer slot index (0-2) of the unlocked frame
			// It's possible another thread is using the buffer for networking or something, loop until we find a valid frame
			bool bLocked = false;
			while (!bLocked)
			{
				for (uint32_t i = 0; i < 3; ++i)
				{
					targetSlot = (base->ActiveWriteFrame + 1 + i) % base->GetTotalFrameCount();
					if (targetSlot == base->ActiveWriteFrame) continue;

					// Try to lock the write frame
					bLocked = base->LockFrameForWrite(targetSlot);
					if (bLocked) break;
				}
			}
		}
		else
		{
			uint32_t spins = 0;
			while (!base->LockFrameForWrite(targetSlot))
			{
				if (++spins > 10'000'000)
				{
					LOG_ENG_ERROR_F("[Temporal] PropagateFrame stuck — frame %u has leaked lock (flags=0x%02x)",
									targetSlot, base->GetFrameHeader(targetSlot)->OwnershipFlags.load(std::memory_order_relaxed));
					return;
				}
			}
		}

		base->LastWrittenFrame = base->ActiveWriteFrame;
		base->ActiveWriteFrame = targetSlot;
		base->PropagateFrameData(base->LastWrittenFrame, targetSlot, counter);
		base->UnlockFrameWrite();
	}
};


// ─────────────────────────────────────────────────────────────────────────────
// Backward-compat aliases — existing code that uses TemporalComponentCache
// continues to compile unchanged.
// ─────────────────────────────────────────────────────────────────────────────
using TemporalComponentCache  = ComponentCache<CacheTier::Temporal>;
using VolatileComponentCache  = ComponentCache<CacheTier::Volatile>;
using UniversalComponentCache = ComponentCache<CacheTier::Universal>;