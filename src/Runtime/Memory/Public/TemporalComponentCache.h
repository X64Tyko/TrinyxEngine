#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
#include <unordered_map>

#include "Types.h"
#include "EngineConfig.h"

enum class SystemID : uint8_t;
class Archetype;

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

	// Camera/View data (replaces FramePacket)
	Matrix4 ViewMatrix;
	Matrix4 ProjectionMatrix;
	Vector3 CameraPosition;

	// Scene/Lighting data
	Vector3 SunDirection;
	Vector3 SunColor;
	float AmbientIntensity;

	// Entity metadata
	uint32_t ActiveEntityCount;
	uint32_t TotalAllocatedEntities;

	// Padding to cache line
	char _padding[64 - (sizeof(std::atomic<uint8_t>) + sizeof(uint8_t) + sizeof(uint32_t) * 3 + sizeof(Vector3) * 3 + sizeof(Matrix4) * 2 + sizeof(float)) % 64];
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
	TemporalFrameHeader* GetFrameHeader(uint32_t frameNum) const
	{
		return FrameHeaders[frameNum % TemporalFrameCount];
	}

	FORCE_INLINE uint32_t GetFrameIndex(uint32_t frameNum) const
	{
		return frameNum % TemporalFrameCount;
	}

	FORCE_INLINE uint32_t GetNextFrame(uint32_t currentFrame) const
	{
		return (currentFrame + 1) % TemporalFrameCount;
	}

	FORCE_INLINE uint32_t GetPrevFrame(uint32_t currentFrame) const
	{
		return (currentFrame + TemporalFrameCount - 1) % TemporalFrameCount;
	}

	// Frame locking for multi-threaded access
	bool TryLockFrameForWrite(uint32_t frameNum);
	bool VerifyFrameReadable(uint32_t frameNum) const;
	void UnlockFrameWrite(uint32_t frameNum);
	bool TryLockFrameForRead(uint32_t frameNum);
	void UnlockFrameRead(uint32_t frameNum);

	// Allocate field array for a chunk across all frames, returns absolute pointer to frame 0 data.
	// Archetype calls this for each temporal field when allocating a new chunk.
	void* AllocateFieldArray(Archetype* owner, struct Chunk* chunk, ComponentTypeID compType,
							 size_t fieldIndex, const char* fieldName, size_t entityCount, size_t fieldSize, SystemID EntitySystemID);

	// Since we're allocating one field at a time and we need them in line we have to advance the allocator manually for now.
	size_t AdvanceAllocator(SystemID EntitySystemID, size_t entityCount, size_t fieldSize);

	void ResetAllocators();

	// Get the stride between frames (for calculating frame N from frame 0 pointer)
	FORCE_INLINE size_t GetFrameStride() const { return sizeof(TemporalFrameHeader) + FrameDataCapacity; }

	// Copy field data from fromFrame into toFrame before dispatch.
	// Called once per logic tick so all FieldProxy writes start from the previous frame's state.
	void PropagateFrame(uint32_t fromFrame, uint32_t toFrame);

	// Get component field data from specific frame.
	// Also returns how many entities are allocated/valid for this field so render can clamp scans safely.
	void* GetFieldData(TemporalFrameHeader* header, ComponentTypeID compType, size_t fieldIndex,
					   size_t& outAllocatedEntities) const;

	uint32_t GetTotalFrameCount() const { return static_cast<uint32_t>(TemporalFrameCount); }
	CacheTier GetTier() const { return Tier_; }

	// Returns a reference to the allocator offset for the given partition.
	// Physics grows right from 0 (Arena 1); Dual grows left from MaxPhysics (Arena 1);
	// Render grows right from MaxPhysics (Arena 2); Logic grows left from MaxCached (Arena 2).
	size_t GetSystemAllocatorIndex(SystemID sysID, size_t size) const;

	size_t AdvanceSystemAllocatorIndex(SystemID sysID, size_t size);

protected:
	// Called by ComponentCacheImpl::Initialize with the correct frame count for the tier.
	void InitializeInternal(const EngineConfig* Config, uint32_t frameCount);

	// Set by ComponentCacheImpl before calling InitializeInternal so GetTier() is valid immediately.
	CacheTier Tier_ = CacheTier::Volatile;

	size_t MaxPhysicsBoundary = 0;
	size_t MaxCachedBoundary  = 0;

	// Per-partition allocation cursors (entity counts, not bytes).
	// Converted to byte offsets at allocation time by multiplying by fieldSize.
	size_t PhysOffset   = 0; // Arena 1, grows right from 0
	size_t DualOffset   = 0; // Arena 1, grows left  from MaxPhysics
	size_t RenderOffset = 0; // Arena 2, grows right from MaxPhysics
	size_t LogicOffset  = 0; // Arena 2, grows left  from MaxCached

private:
	// Pre-computed field allocation zones (field-major ordering across all archetypes)
	struct FieldAllocationInfo
	{
		ComponentTypeID CompType = 0;
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
		InitializeInternal(Config, static_cast<Derived*>(this)->GetFrameCount(Config));
	}
};

// ─────────────────────────────────────────────────────────────────────────────
// Concrete cache type.  Registry instantiates these; Archetype/LogicThread/
// RenderThread use the ComponentCacheBase* pointer.
//
// The 3 CRTP overrides that differ between tiers:
//   GetFrameCount  — Volatile: 5 (fixed), Temporal: Config->TemporalFrameCount
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
			return 5;
		}
		else
		{
#ifndef TNX_ENABLE_ROLLBACK
			return 5;
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
	
	size_t GetSystemAllocatorIndex([[maybe_unused]] SystemID sysID, [[maybe_unused]] size_t size) const
	{
		if constexpr (Tier == CacheTier::Universal || Tier == CacheTier::None) return this->PhysOffset;

		return ComponentCacheBase::GetSystemAllocatorIndex(sysID, size);
	}
};


// ─────────────────────────────────────────────────────────────────────────────
// Backward-compat aliases — existing code that uses TemporalComponentCache
// continues to compile unchanged.
// ─────────────────────────────────────────────────────────────────────────────
using TemporalComponentCache  = ComponentCache<CacheTier::Temporal>;
using VolatileComponentCache  = ComponentCache<CacheTier::Volatile>;
using UniversalComponentCache = ComponentCache<CacheTier::Universal>;