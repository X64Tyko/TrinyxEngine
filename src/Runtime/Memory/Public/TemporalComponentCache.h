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
using FnPropagateFramePtr    = void (*)(class ComponentCacheBase*, TrinyxJobs::JobCounter&);

/// @brief Cache-line-aligned per-frame metadata stored at the start of each ring-buffer slot.
///
/// Holds ownership flags for concurrent read/write coordination, the simulation frame number,
/// camera and lighting state for the render thread, and (under rollback) the input snapshot
/// needed to replay a frame deterministically.
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
	SimFloat InputViewYaw; // absolute control rotation for this frame
	SimFloat InputViewPitch;
#endif

	// No manual tail padding: alignas(64) rounds sizeof up to whole cache lines.
};

/// @brief Non-template concrete base for all SoA ring-buffer caches.
///
/// Owns all slab memory, frame headers, field allocation tables, and the frame
/// locking protocol. @c Archetype, @c LogicThread, and @c RenderThread all
/// store a @c ComponentCacheBase* so they are decoupled from the tier template.
///
/// No virtual functions — @c Initialize is on the CRTP middle layer; all runtime
/// methods here are concrete and shared between Volatile and Temporal tiers.
///
/// @see ComponentCache for the concrete typed instantiation.
class ComponentCacheBase
{
public:
	ComponentCacheBase();
	~ComponentCacheBase();

	/// @brief O(1) frame header access. External systems are responsible for locking.
	/// @param frameNum Ring-buffer slot index; -1 returns the active write frame.
	TemporalFrameHeader* GetFrameHeader(int32_t frameNum = -1) const
	{
		frameNum = frameNum == -1 ? ActiveWriteFrame : frameNum;
		return FrameHeaders[frameNum % TemporalFrameCount];
	}

	/// @brief Try to claim the next write slot. After propagation the frame remains locked until propagated again.
	/// @param[out] outWriteFrame The slot index being written to (for field data access).
	bool TryLockFrameForWrite(uint32_t& outWriteFrame);
	bool VerifyFrameReadable(uint32_t bufferIndex) const; ///< @return @c true if no writer holds @p bufferIndex.
	void UnlockFrameWrite();                              ///< Release the current write lock.
	bool TryLockFrameForRead(uint32_t frameNum);          ///< Try to acquire a shared read lock on @p frameNum.
	void UnlockFrameRead(uint32_t frameNum);              ///< Release a previously acquired read lock.

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

	// Phase-2 slab defrag: called by TrimTailChunks before the chunk struct is freed.
	// Records per-field slab regions into FreedChunkSlabs and decrements CurrentUsed,
	// making the space available for reuse by the next AllocateChunk for the same archetype.
	void NotifyChunkFreed(Chunk* chunk);

	// Called at the top of AllocateChunk (before AllocateFieldArray).
	// If a previously freed slab from the same archetype is available, wires FieldPtrs[] in
	// newChunk for every temporal/volatile field that belongs to this cache, re-registers the
	// allocations under newChunk, and returns the recycled CacheIndexStart.
	// Returns SIZE_MAX when no suitable freed slab exists (caller must use AllocateFieldArray).
	size_t TryReuseFreedSlab(Chunk* newChunk, Archetype* owner);

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
	// Advance a frame-0 base pointer to the ring buffer slot that holds a specific
	// simulation frame number. The caller is responsible for ensuring absoluteSimFrame
	// is within the live ring window (i.e. not overwritten by newer frames).
	FORCE_INLINE void* GetFramePtrAtAbsoluteFrame(void* frame0Base, uint32_t absoluteSimFrame) const
	{
		uint32_t slot = absoluteSimFrame % static_cast<uint32_t>(TemporalFrameCount);
		return static_cast<uint8_t*>(frame0Base) + slot * GetFrameStride();
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
	// During rollback resim: advance the ring frame pointer and scatter-copy only entities
	// with DirtiedFrameBit set. Non-dirty entities in the destination slot retain their
	// original-timeline data — no full memcpy is performed. Caller's WaitForCounter
	// returns immediately (no async jobs dispatched).
	void PropagateFrameResim(TrinyxJobs::JobCounter& counter);

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
	FnPropagateFramePtr FnPropagateFrame       = nullptr;

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

	// Freed slab regions available for reuse when a new chunk of the same archetype is allocated.
	// Populated by NotifyChunkFreed; consumed by TryReuseFreedSlab.
	struct FreedChunkSlab
	{
		Archetype* Owner;
		size_t CacheStart; // chunk->Header.CacheIndexStart at time of free

		struct FieldRegion
		{
			uint16_t TableIndex;    // FieldAllocations[] index
			uint8_t FieldSlotIndex; // Chunk::Header::FieldPtrs[] slot to wire on reuse
			size_t OffsetInZone;    // Byte offset within this field's slab zone
			size_t Size;            // Bytes (= AlignSize(entitiesPerChunk * fieldSize))
		};
		std::vector<FieldRegion> Fields;
	};
	std::vector<FreedChunkSlab> FreedChunkSlabs;

	// One large slab storing multiple frames of history
	void* SlabPtr             = nullptr;
	size_t FrameDataCapacity  = 0; // Max size per frame (all field allocations)
	size_t TemporalFrameCount = 0; // Number of frames stored in this slab
	size_t TotalSlabSize      = 0; // Total allocation size

	// O(1) frame access array
	std::vector<TemporalFrameHeader*> FrameHeaders;

	static size_t AlignSize(size_t size);
};

/// @brief CRTP middle layer that routes @c Initialize() to tier-specific overrides.
///
/// Each derived class (@c ComponentCache<Tier>) provides:
/// - @c GetFrameCount(Config) — how many ring-buffer frames to allocate.
/// - @c GetCacheTier() — the @c CacheTier enum value for this instance.
/// - @c GetLabel() — human-readable name for logging.
///
/// All data and frame/lock/alloc methods live in @ref ComponentCacheBase.
template <typename Derived>
class ComponentCacheImpl : public ComponentCacheBase
{
public:
	void Initialize(const EngineConfig* Config);

protected:
	friend Derived;
};

/// @brief Concrete SoA ring-buffer cache parameterised by storage tier.
///
/// @ref Registry instantiates @c ComponentCache<Volatile> and
/// @c ComponentCache<Temporal>. All callers use the @ref ComponentCacheBase*
/// interface so they are decoupled from the tier template.
///
/// CRTP overrides per tier:
/// - @c GetFrameCount — Volatile: 3 (triple-buffer); Temporal: @c Config->TemporalFrameCount.
/// - @c GetCacheTier — returns the @c CacheTier enum value.
/// - @c GetLabel — @c "Volatile" or @c "Temporal" for logging.
template <CacheTier Tier>
class ComponentCache final : public ComponentCacheImpl<ComponentCache<Tier>>
{
public:
	uint32_t GetFrameCount(const EngineConfig* Config) const;

	static constexpr CacheTier GetCacheTier() { return Tier; }

	static constexpr const char* GetLabel()
	{
		if constexpr (Tier == CacheTier::Volatile) return "Volatile";
		if constexpr (Tier == CacheTier::Universal)
			return "Universal";
		else
			return "Temporal";
	}

	// Static implementations for function pointers
	static uint32_t GetNextWriteFrameImpl(const ComponentCacheBase* base);

	static void PropagateFrameImpl(ComponentCacheBase* base, TrinyxJobs::JobCounter& counter);
};


// ─────────────────────────────────────────────────────────────────────────────
// Backward-compat aliases — existing code that uses TemporalComponentCache
// continues to compile unchanged.
// ─────────────────────────────────────────────────────────────────────────────
using TemporalComponentCache  = ComponentCache<CacheTier::Temporal>;
using VolatileComponentCache  = ComponentCache<CacheTier::Volatile>;
using UniversalComponentCache = ComponentCache<CacheTier::Universal>;

// Closed set of instantiations — member bodies and explicit instantiations live in TemporalComponentCache.cpp.
extern template class ComponentCacheImpl<ComponentCache<CacheTier::Volatile>>;
extern template class ComponentCacheImpl<ComponentCache<CacheTier::Temporal>>;
extern template class ComponentCacheImpl<ComponentCache<CacheTier::Universal>>;
extern template class ComponentCache<CacheTier::Volatile>;
extern template class ComponentCache<CacheTier::Temporal>;
extern template class ComponentCache<CacheTier::Universal>;
