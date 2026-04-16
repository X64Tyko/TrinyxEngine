#include "TemporalComponentCache.h"

#include <cassert>
#include <cstring>
#include <immintrin.h>
#include "Archetype.h"
#include "EngineConfig.h"
#include "FieldMeta.h"
#include "MemoryDefines.h"
#include "Schema.h"
#include "TrinyxJobs.h"
#include "Types.h"

ComponentCacheBase::ComponentCacheBase()
{
}

ComponentCacheBase::~ComponentCacheBase()
{
	LOG_ENG_INFO_F("Destroying %s Component Slab with %zu bytes",
				   Tier_ == CacheTier::Volatile ? "Volatile" : "Temporal", TotalSlabSize);

#ifdef _MSC_VER
	_aligned_free(SlabPtr);
#else
	free(SlabPtr);
#endif
}

size_t ComponentCacheBase::GetSystemAllocatorIndex(SystemID sysID, size_t size) const
{
	if (Equal(sysID, SystemID::Dual)) return MaxRenderableBoundary - DualOffset - size;
	if (Equal(sysID, SystemID::Physics)) return MaxRenderableBoundary + PhysOffset;
	if (Equal(sysID, SystemID::Render)) return RenderOffset;
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
		retSize    = MaxRenderableBoundary - DualOffset;
	}
	else if (Equal(sysID, SystemID::Physics))
	{
		retSize    = MaxRenderableBoundary + PhysOffset;
		PhysOffset += size;
	}
	else if (Equal(sysID, SystemID::Render))
	{
		retSize      = RenderOffset;
		RenderOffset += size;
	}
	else
	{
		LogicOffset += size;
		retSize     = MaxCachedBoundary - LogicOffset;
	}

	assert(RenderOffset + DualOffset <= MaxRenderableBoundary);
	assert(PhysOffset + LogicOffset <= MaxCachedBoundary);

	return retSize / 4; // hardcoded 32-bit value...
}

void ComponentCacheBase::InitializeInternal(const EngineConfig* Config, uint32_t frameCount)
{
	auto& CFR = ReflectionRegistry::Get();

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
	MaxRenderableBoundary = static_cast<size_t>(FieldAllocations[ValidFields[0]].TotalCapacity * (static_cast<double>(Config->MAX_RENDERABLE_ENTITIES) / Config->MAX_CACHED_ENTITIES));

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
		LOG_ENG_ERROR_F("Failed to allocate memory for %s ComponentCache slab: %zu bytes",
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

	LOG_ENG_INFO_F("Initialized %s ComponentCache: %zu fields, %zu frames × %zu bytes = %zu total bytes",
				   Tier_ == CacheTier::Volatile ? "Volatile" : "Temporal",
			   ValidFields.size(), TemporalFrameCount, frameStride, TotalSlabSize);
}

bool ComponentCacheBase::LockFrameForWrite(uint32_t WriteFrame)
{
	
	// We still want to make sure only 1 thread is trying to lock the write frame at a time
	TemporalFrameHeader* header = GetFrameHeader(WriteFrame);

	// Try to acquire write lock - frame must be completely unlocked (no readers or writers)
	// 0x01 = LOGIC_WRITING, 0x02 = RENDER_READING, 0x04 = NETWORK_READING, 0x08 = DEFRAG_LOCKED
	uint8_t expected = 0;
	return header->OwnershipFlags.compare_exchange_strong(expected, 0x01, std::memory_order_acquire);
}

void* ComponentCacheBase::AllocateFieldArray(Archetype* owner, Chunk* chunk,
											 CacheSlotID cacheSlot, size_t fieldIndex,
											 const char* fieldName, size_t entityCount, size_t fieldSize, SystemID EntitySystemID)
{
	// Direct O(1) lookup into flat table
	if (cacheSlot >= MAX_COMPONENTS || fieldIndex >= MAX_TEMPORAL_FIELDS_PER_COMPONENT)
	{
		LOG_ENG_ERROR_F("ComponentCacheBase: Invalid cache slot %u or field index %zu", cacheSlot, fieldIndex);
		return nullptr;
	}

	const size_t tableIndex   = static_cast<size_t>(cacheSlot) * MAX_TEMPORAL_FIELDS_PER_COMPONENT + fieldIndex;
	FieldAllocationInfo& info = FieldAllocations[tableIndex];

	if (!info.bValid)
	{
		LOG_ENG_ERROR_F("ComponentCacheBase: Field %s (component %u, field %zu) not initialized",
						fieldName, cacheSlot, fieldIndex);
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
		LOG_ENG_ERROR_F("ComponentCacheBase: Out of space for field %s (component %u, field %zu)",
						fieldName, cacheSlot, fieldIndex);
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

void ComponentCacheBase::ClearFrameData()
{
	for (size_t i = 0; i < TemporalFrameCount; ++i)
	{
		TemporalFrameHeader* Header = FrameHeaders[i];
		std::memset(reinterpret_cast<uint8_t*>(Header) + sizeof(TemporalFrameHeader), 0, FrameDataCapacity);
	}
}

void* ComponentCacheBase::GetFieldData(TemporalFrameHeader* header, CacheSlotID cacheSlot,
									   size_t fieldIndex) const
{
	if (!header) return nullptr;

	if (cacheSlot >= MAX_COMPONENTS || fieldIndex >= MAX_TEMPORAL_FIELDS_PER_COMPONENT) return nullptr;

	const size_t tableIndex         = static_cast<size_t>(cacheSlot) * MAX_TEMPORAL_FIELDS_PER_COMPONENT + fieldIndex;
	const FieldAllocationInfo& info = FieldAllocations[tableIndex];

	if (!info.bValid) return nullptr;

	uint8_t* frameData = reinterpret_cast<uint8_t*>(header) + sizeof(TemporalFrameHeader);
	return frameData + info.OffsetInFrame;
}


bool ComponentCacheBase::TryLockFrameForWrite(uint32_t& outWriteFrame)
{
	outWriteFrame = ActiveWriteFrame;
	// We still want to make sure only 1 thread is trying to lock the write frame at a time
	TemporalFrameHeader* header = GetFrameHeader(ActiveWriteFrame);

	// Try to acquire write lock - frame must be completely unlocked (no readers or writers)
	// 0x01 = LOGIC_WRITING, 0x02 = RENDER_READING, 0x04 = NETWORK_READING, 0x08 = DEFRAG_LOCKED
	uint8_t expected = 0;
	return header->OwnershipFlags.compare_exchange_strong(expected, 0x01, std::memory_order_acquire);
}

bool ComponentCacheBase::VerifyFrameReadable(uint32_t bufferIndex) const
{
	const TemporalFrameHeader* header = GetFrameHeader(bufferIndex);

	// Frame is readable if no write lock is held (0x01 = LOGIC_WRITING)
	uint8_t flags = header->OwnershipFlags.load(std::memory_order_acquire);
	return (flags & 0x01) == 0;
}

void ComponentCacheBase::UnlockFrameWrite()
{
	TemporalFrameHeader* header = GetFrameHeader(ActiveWriteFrame);

	// Release write lock (clear LOGIC_WRITING bit)
	header->OwnershipFlags.fetch_and(~0x01, std::memory_order_release);
}

bool ComponentCacheBase::TryLockFrameForRead(uint32_t frameNum)
{
	assert(static_cast<int32_t>(frameNum) != -1 && "TryLockFrameForRead: caller must pass a concrete frame number, not -1");
	uint32_t readFrame          = frameNum;
	TemporalFrameHeader* header = GetFrameHeader(readFrame);

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
	assert(static_cast<int32_t>(frameNum) != -1 && "UnlockFrameRead: caller must pass the same concrete frame number used to lock");
	uint32_t readFrame          = frameNum;
	TemporalFrameHeader* header = GetFrameHeader(readFrame);

	// Release read lock (clear RENDER_READING bit)
	header->OwnershipFlags.fetch_and(~0x02, std::memory_order_release);
}

void ComponentCacheBase::PropagateFrameData(uint32_t fromFrame, uint32_t toFrame, TrinyxJobs::JobCounter& counter)
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
		{0, RenderOffset},                                             // Render (Arena 1, → from 0)
		{MaxRenderableBoundary - DualOffset, DualOffset + PhysOffset}, // Dual + Phys (adjacent at boundary)
		{MaxCachedBoundary - LogicOffset, LogicOffset},                // Logic (Arena 2, ← from MaxCached)
	};

	for (uint16_t idx : ValidFields)
	{
		const size_t zoneBase = FieldAllocations[idx].OffsetInFrame;
		for (const auto& r : regions)
		{
			size_t off = zoneBase + r.offset;
			size_t sz  = r.size;
			TrinyxJobs::Dispatch([off, sz, readData, writeData](uint32_t)
			{
				if (sz == 0) return;

				const auto* src       = reinterpret_cast<const __m256i*>(readData + off);
				auto* dst             = reinterpret_cast<__m256i*>(writeData + off);
				const size_t chunks   = sz / 32;
				const size_t leftover = sz % 32;
				for (size_t i = 0; i < chunks; ++i) _mm256_stream_si256(dst + i, _mm256_stream_load_si256(src + i));
				if (leftover) std::memcpy(writeData + off + chunks * 32, readData + off + chunks * 32, leftover);
			}, &counter, TrinyxJobs::Queue::Logic);
		}
	}
}

size_t ComponentCacheBase::AlignSize(size_t size)
{
	return (size + FIELD_ARRAY_ALIGNMENT - 1) & ~(FIELD_ARRAY_ALIGNMENT - 1);
}

#ifdef TNX_ENABLE_ROLLBACK
std::vector<ComponentCacheBase::FieldCompareInfo> ComponentCacheBase::GetValidFieldInfos() const
{
	std::vector<FieldCompareInfo> result;
	result.reserve(ValidFields.size());
	for (uint16_t idx : ValidFields)
	{
		const auto& fa = FieldAllocations[idx];
		result.push_back({fa.CompType, fa.FieldIndex, fa.FieldName,
						  fa.OffsetInFrame, fa.CurrentUsed, fa.FieldSize});
	}
	return result;
}
#endif
