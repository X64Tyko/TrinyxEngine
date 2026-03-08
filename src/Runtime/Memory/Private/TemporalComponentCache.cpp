#include "TemporalComponentCache.h"

#include <cassert>
#include <cstring>
#include <immintrin.h>
#include "Archetype.h"
#include "EngineConfig.h"
#include "FieldMeta.h"
#include "MemoryDefines.h"
#include "Schema.h"
#include "Types.h"

ComponentCacheBase::ComponentCacheBase()
{
}

ComponentCacheBase::~ComponentCacheBase()
{
	LOG_INFO_F("Destroying %s Component Slab with %zu bytes",
			   Tier_ == CacheTier::Volatile ? "Volatile" : "Temporal", TotalSlabSize);

#ifdef _MSC_VER
	_aligned_free(SlabPtr);
#else
	free(SlabPtr);
#endif
}

size_t ComponentCacheBase::GetSystemAllocatorIndex(SystemID sysID, size_t size) const
{
	if (Equal(sysID, SystemID::Dual)) return MaxPhysicsBoundary - DualOffset - size;
	if (Equal(sysID, SystemID::Render)) return MaxPhysicsBoundary + RenderOffset;
	if (Equal(sysID, SystemID::Physics)) return PhysOffset;
	return MaxCachedBoundary - LogicOffset - size;
}

size_t ComponentCacheBase::AdvanceSystemAllocatorIndex(SystemID sysID, size_t size)
{
	size_t retSize = 0;

	if (Tier_ == CacheTier::None || Tier_ == CacheTier::Universal)
	{
		PhysOffset += size;
		retSize    = PhysOffset;
	}

	else if (Equal(sysID, SystemID::Dual))
	{
		DualOffset += size;
		retSize    = MaxPhysicsBoundary - DualOffset;
	}
	else if (Equal(sysID, SystemID::Render))
	{
		retSize      = MaxPhysicsBoundary - RenderOffset;
		RenderOffset += size;
	}
	else if (Equal(sysID, SystemID::Physics))
	{
		retSize    = PhysOffset;
		PhysOffset += size;
	}
	else
	{
		LogicOffset += size;
		retSize     = MaxCachedBoundary - LogicOffset;
	}

	assert(PhysOffset + DualOffset <= MaxPhysicsBoundary);
	assert(RenderOffset + LogicOffset <= MaxCachedBoundary);

	return retSize / 4; // hardcoded 32-bit value...
}

void ComponentCacheBase::InitializeInternal(const EngineConfig* Config, uint32_t frameCount)
{
	ComponentFieldRegistry& CFR = ComponentFieldRegistry::Get();

	// Pre-compute field allocation layout (field-major ordering)
	// For each component in this tier, for each field, reserve a contiguous zone.
	size_t totalFrameSize = 0;

	for (auto& [typeID, meta] : CFR.GetAllComponents())
	{
		if (meta.TemporalTier != Tier_) continue;

		// For each field in this component
		for (size_t fieldIdx = 0; fieldIdx < meta.Fields.size(); ++fieldIdx)
		{
			const FieldMeta& field            = meta.Fields[fieldIdx];
			const ComponentTypeID cacheSlotID = meta.CacheSlotIndex;

			// Zone capacity = actual data + worst-case alignment padding.
			// baseSize covers every possible entity; maxAlignPadding covers chunk-start alignment.
			size_t baseSize     = static_cast<size_t>(Config->MAX_CACHED_ENTITIES) * field.Size;
			size_t maxFieldSize = baseSize; // No padding,

			// Use flat O(1) table instead of vector
			if (cacheSlotID < MAX_COMPONENTS && fieldIdx < MAX_TEMPORAL_FIELDS_PER_COMPONENT)
			{
				const size_t tableIndex      = static_cast<size_t>(cacheSlotID) * MAX_TEMPORAL_FIELDS_PER_COMPONENT + fieldIdx;
				FieldAllocations[tableIndex] = {
					typeID,
					fieldIdx,
					field.Name,
					totalFrameSize,
					maxFieldSize,
					0, // CurrentUsed starts at 0
					field.Size,
					true // bValid
				};

				// Track valid entries compactly — avoids scanning all 16k slots on PropagateFrame / logging
				ValidFields.push_back(tableIndex);

				totalFrameSize += maxFieldSize;
			}
		}
	}

	MaxCachedBoundary  = FieldAllocations[ValidFields[0]].TotalCapacity;
	MaxPhysicsBoundary = static_cast<size_t>(FieldAllocations[ValidFields[0]].TotalCapacity * (static_cast<double>(Config->MAX_PHYSICS_ENTITIES) / Config->MAX_CACHED_ENTITIES));

	const size_t HeaderSize = sizeof(TemporalFrameHeader);
	FrameDataCapacity       = totalFrameSize;
	TemporalFrameCount      = frameCount;
	TotalSlabSize           = (FrameDataCapacity + HeaderSize) * TemporalFrameCount;

	// Aligned allocation for SIMD safety
#ifdef _MSC_VER
	SlabPtr = static_cast<uint8_t*>(_aligned_malloc(TotalSlabSize, 64));
#else
	SlabPtr = static_cast<uint8_t*>(aligned_alloc(64, TotalSlabSize));
#endif

	if (SlabPtr == nullptr)
	{
		LOG_ERROR_F("Failed to allocate memory for %s ComponentCache slab: %zu bytes",
					Tier_ == CacheTier::Volatile ? "Volatile" : "Temporal", TotalSlabSize);
		return;
	}

	// Zero-initialize entire slab (critical for Release builds where uninitialized memory isn't zeroed)
	std::memset(SlabPtr, 0, TotalSlabSize);

	// Build frame header pointer array for O(1) access
	FrameHeaders.reserve(TemporalFrameCount);
	uint8_t* currentFramePtr = static_cast<uint8_t*>(SlabPtr);
	const size_t frameStride = HeaderSize + FrameDataCapacity;

	for (size_t i = 0; i < TemporalFrameCount; ++i)
	{
		auto* header = reinterpret_cast<TemporalFrameHeader*>(currentFramePtr);

		// Initialize frame header
		header->OwnershipFlags.store(0, std::memory_order_release); // Unlocked
		header->FrameNumber            = 0;
		header->ActiveEntityCount      = 0;
		header->TotalAllocatedEntities = 0;

		FrameHeaders.push_back(header);
		currentFramePtr += frameStride;
	}

	// ValidFields was populated above alongside FieldAllocations — no 16k scan needed.

	LOG_INFO_F("Initialized %s ComponentCache: %zu fields, %zu frames × %zu bytes = %zu total bytes",
			   Tier_ == CacheTier::Volatile ? "Volatile" : "Temporal",
			   ValidFields.size(), TemporalFrameCount, frameStride, TotalSlabSize);
}

void* ComponentCacheBase::AllocateFieldArray(Archetype* owner, Chunk* chunk,
											 ComponentTypeID compType, size_t fieldIndex,
											 const char* fieldName, size_t entityCount, size_t fieldSize, SystemID EntitySystemID)
{
	// Direct O(1) lookup into flat table
	if (compType >= MAX_COMPONENTS || fieldIndex >= MAX_TEMPORAL_FIELDS_PER_COMPONENT)
	{
		LOG_ERROR_F("ComponentCacheBase: Invalid component type %u or field index %zu", compType, fieldIndex);
		return nullptr;
	}

	const size_t tableIndex   = static_cast<size_t>(compType) * MAX_TEMPORAL_FIELDS_PER_COMPONENT + fieldIndex;
	FieldAllocationInfo& info = FieldAllocations[tableIndex];

	if (!info.bValid)
	{
		LOG_ERROR_F("ComponentCacheBase: Field %s (component %u, field %zu) not initialized",
					fieldName, compType, fieldIndex);
		return nullptr;
	}

	// Calculate aligned size for this chunk's allocation
	size_t allocSize = AlignSize(entityCount * fieldSize);

	// Check capacity
	if (info.CurrentUsed + allocSize > info.TotalCapacity) [[unlikely]]
	{
		if (Tier_ == CacheTier::Universal)
		{
			// resize?
		}
		LOG_ERROR_F("ComponentCacheBase: Out of space for field %s (component %u, field %zu)",
					fieldName, compType, fieldIndex);
		return nullptr;
	}

	// Allocate from this field's zone
	size_t offsetInFieldZone = GetSystemAllocatorIndex(EntitySystemID, allocSize);
	info.CurrentUsed         += allocSize;

	// Store allocation metadata for defrag
	ActiveAllocations.push_back({
		owner,
		chunk,
		tableIndex,
		offsetInFieldZone,
		allocSize
	});

	// Return absolute pointer to frame 0's data for this allocation
	uint8_t* frame0Data = static_cast<uint8_t*>(SlabPtr) + sizeof(TemporalFrameHeader);
	return frame0Data + info.OffsetInFrame + offsetInFieldZone;
}

size_t ComponentCacheBase::AdvanceAllocator(SystemID EntitySystemID, size_t entityCount, size_t fieldSize)
{
	// Calculate aligned size for this chunk's allocation
	size_t allocSize = AlignSize(entityCount * fieldSize);
	return AdvanceSystemAllocatorIndex(EntitySystemID, allocSize);
}

void ComponentCacheBase::ResetAllocators()
{
	PhysOffset   = 0;
	RenderOffset = 0;
	DualOffset   = 0;
	LogicOffset  = 0;
}

void* ComponentCacheBase::GetFieldData(TemporalFrameHeader* header, ComponentTypeID compType,
									   size_t fieldIndex, size_t& outAllocatedEntities) const
{
	outAllocatedEntities = 0;

	if (!header) return nullptr;

	if (compType >= MAX_COMPONENTS || fieldIndex >= MAX_TEMPORAL_FIELDS_PER_COMPONENT) return nullptr;

	const size_t tableIndex         = static_cast<size_t>(compType) * MAX_TEMPORAL_FIELDS_PER_COMPONENT + fieldIndex;
	const FieldAllocationInfo& info = FieldAllocations[tableIndex];

	if (!info.bValid) return nullptr;

	if (info.FieldSize != 0)
	{
		outAllocatedEntities = info.CurrentUsed / info.FieldSize;
	}

	uint8_t* frameData = reinterpret_cast<uint8_t*>(header) + sizeof(TemporalFrameHeader);
	return frameData + info.OffsetInFrame;
}


bool ComponentCacheBase::TryLockFrameForWrite(uint32_t frameNum)
{
	TemporalFrameHeader* header = GetFrameHeader(frameNum);

	// Try to acquire write lock - frame must be completely unlocked (no readers or writers)
	// 0x01 = LOGIC_WRITING, 0x02 = RENDER_READING, 0x04 = NETWORK_READING, 0x08 = DEFRAG_LOCKED
	uint8_t expected = 0;
	return header->OwnershipFlags.compare_exchange_strong(expected, 0x01, std::memory_order_acquire);
}

bool ComponentCacheBase::VerifyFrameReadable(uint32_t frameNum) const
{
	const TemporalFrameHeader* header = GetFrameHeader(frameNum);

	// Frame is readable if no write lock is held (0x01 = LOGIC_WRITING)
	uint8_t flags = header->OwnershipFlags.load(std::memory_order_acquire);
	return (flags & 0x01) == 0;
}

void ComponentCacheBase::UnlockFrameWrite(uint32_t frameNum)
{
	TemporalFrameHeader* header = GetFrameHeader(frameNum);

	// Release write lock (clear LOGIC_WRITING bit)
	header->OwnershipFlags.fetch_and(~0x01, std::memory_order_release);
}

bool ComponentCacheBase::TryLockFrameForRead(uint32_t frameNum)
{
	TemporalFrameHeader* header = GetFrameHeader(frameNum);

	// Frame is readable if no write lock is held (0x01 = LOGIC_WRITING)
	uint8_t flags = header->OwnershipFlags.load(std::memory_order_acquire);
	if ((flags & 0x01) == 0)
	{
		header->OwnershipFlags.fetch_or(0x02, std::memory_order_release);
		return true;
	}
	return false;
}

void ComponentCacheBase::UnlockFrameRead(uint32_t frameNum)
{
	TemporalFrameHeader* header = GetFrameHeader(frameNum);

	// Release read lock (clear RENDER_READING bit)
	header->OwnershipFlags.fetch_and(~0x02, std::memory_order_release);
}

void ComponentCacheBase::PropagateFrame(uint32_t fromFrame, uint32_t toFrame)
{
	if (FrameDataCapacity == 0) return;

	uint8_t* readData  = reinterpret_cast<uint8_t*>(GetFrameHeader(fromFrame)) + sizeof(TemporalFrameHeader);
	uint8_t* writeData = reinterpret_cast<uint8_t*>(GetFrameHeader(toFrame)) + sizeof(TemporalFrameHeader);

	TNX_ZONE_MEDIUM_NC("Memcpy Frame", TNX_COLOR_LOGIC)

	// Copy only live partition ranges — skips dead arena gaps entirely.
	// NT stores bypass L3; destination frame won't be read until next tick.
	// Each partition region must be applied to EVERY field zone in the slab,
	// because each temporal field has its own zone at a unique OffsetInFrame.
	struct Region
	{
		size_t offset;
		size_t size;
	};
	const Region regions[3] = {
		{0, PhysOffset},                                              // Physics
		{MaxPhysicsBoundary - DualOffset, DualOffset + RenderOffset}, // Dual + Render (adjacent)
		{MaxCachedBoundary - LogicOffset, LogicOffset},               // Logic
	};

	auto NTCopy = [&](size_t off, size_t sz)
	{
		if (sz == 0) return;
		const auto* src       = reinterpret_cast<const __m256i*>(readData + off);
		auto* dst             = reinterpret_cast<__m256i*>(writeData + off);
		const size_t chunks   = sz / 32;
		const size_t leftover = sz % 32;
		for (size_t i = 0; i < chunks; ++i) _mm256_stream_si256(dst + i, _mm256_loadu_si256(src + i));
		if (leftover) std::memcpy(writeData + off + chunks * 32, readData + off + chunks * 32, leftover);
	};

	for (uint16_t idx : ValidFields)
	{
		const size_t zoneBase = FieldAllocations[idx].OffsetInFrame;
		for (const auto& r : regions) NTCopy(zoneBase + r.offset, r.size);
	}
	_mm_sfence();
}

size_t ComponentCacheBase::AlignSize(size_t size)
{
	return (size + FIELD_ARRAY_ALIGNMENT - 1) & ~(FIELD_ARRAY_ALIGNMENT - 1);
}
