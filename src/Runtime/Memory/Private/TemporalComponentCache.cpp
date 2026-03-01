#include "TemporalComponentCache.h"

#include <cstring>
#include "Archetype.h"
#include "EngineConfig.h"
#include "FieldMeta.h"
#include "MemoryDefines.h"
#include "Schema.h"
#include "Types.h"

TemporalComponentCache::TemporalComponentCache()
{
}

TemporalComponentCache::~TemporalComponentCache()
{
	LOG_INFO_F("Destroying Temporal Component Slab with %zu bytes", TotalSlabSize);

#ifdef _MSC_VER
	_aligned_free(SlabPtr);
#else
	free(SlabPtr);
#endif
}

void TemporalComponentCache::Initialize(const EngineConfig* Config)
{
	ComponentFieldRegistry& CFR = ComponentFieldRegistry::Get();

	// Pre-compute field allocation layout (field-major ordering)
	// For each temporal component, for each field, reserve a zone
	size_t totalFrameSize = 0;

	for (auto& [typeID, meta] : CFR.GetAllComponents())
	{
		if (!meta.IsTemporal) continue;

		// For each field in this component
		for (size_t fieldIdx = 0; fieldIdx < meta.Fields.size(); ++fieldIdx)
		{
			const FieldMeta& field = meta.Fields[fieldIdx];

			// Calculate max capacity for this field across all archetypes
			// We need: (MaxDynamicEntities * fieldSize) + padding for chunk alignment
			// Worst case: smallest EntitiesPerChunk across all archetypes = 1 entity/chunk
			// Therefore: maxChunks = MaxDynamicEntities (if every archetype has 1 entity/chunk)
			// Each chunk allocation is aligned, wasting up to (ALIGNMENT - 1) bytes
			size_t baseSize     = Config->MaxDynamicEntities * field.Size;
			size_t maxChunks    = Config->MaxDynamicEntities; // Worst case: 1 entity per chunk
			size_t maxPadding   = maxChunks * (FIELD_ARRAY_ALIGNMENT - 1);
			size_t maxFieldSize = baseSize + maxPadding;

			// Use flat O(1) table instead of vector
			if (typeID < MAX_COMPONENTS && fieldIdx < MAX_TEMPORAL_FIELDS_PER_COMPONENT)
			{
				const size_t tableIndex      = static_cast<size_t>(typeID) * MAX_TEMPORAL_FIELDS_PER_COMPONENT + fieldIdx;
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

				totalFrameSize += maxFieldSize;
			}
		}
	}

	const size_t HeaderSize = sizeof(TemporalFrameHeader);
	FrameDataCapacity       = totalFrameSize;
	TemporalFrameCount      = Config->TemporalFrameCount;
	TotalSlabSize           = (FrameDataCapacity + HeaderSize) * TemporalFrameCount;

	// Aligned allocation for SIMD safety
#ifdef _MSC_VER
	SlabPtr = static_cast<uint8_t*>(_aligned_malloc(TotalSlabSize, 64));
#else
	SlabPtr = static_cast<uint8_t*>(aligned_alloc(64, TotalSlabSize));
#endif

	if (SlabPtr == nullptr)
	{
		LOG_ERROR_F("Failed to allocate memory for TemporalComponentCache slab: %zu bytes", TotalSlabSize);
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

	// Count valid fields
	size_t validFieldCount = 0;
	for (const auto& info : FieldAllocations)
	{
		if (info.bValid) ++validFieldCount;
	}

	LOG_INFO_F("Initialized TemporalComponentCache: %zu fields, %zu frames × %zu bytes = %zu total bytes",
			   validFieldCount, TemporalFrameCount, frameStride, TotalSlabSize);
}

void* TemporalComponentCache::AllocateFieldArray(Archetype* owner, Chunk* chunk,
												 ComponentTypeID compType, size_t fieldIndex,
												 const char* fieldName, size_t entityCount, size_t fieldSize)
{
	// Direct O(1) lookup into flat table
	if (compType >= MAX_COMPONENTS || fieldIndex >= MAX_TEMPORAL_FIELDS_PER_COMPONENT)
	{
		LOG_ERROR_F("TemporalComponentCache: Invalid component type %u or field index %zu", compType, fieldIndex);
		return nullptr;
	}

	const size_t tableIndex   = static_cast<size_t>(compType) * MAX_TEMPORAL_FIELDS_PER_COMPONENT + fieldIndex;
	FieldAllocationInfo& info = FieldAllocations[tableIndex];

	if (!info.bValid)
	{
		LOG_ERROR_F("TemporalComponentCache: Field %s (component %u, field %zu) not initialized",
					fieldName, compType, fieldIndex);
		return nullptr;
	}

	// Calculate aligned size for this chunk's allocation
	size_t allocSize = AlignSize(entityCount * fieldSize);

	// Check capacity
	if (info.CurrentUsed + allocSize > info.TotalCapacity)
	{
		LOG_ERROR_F("TemporalComponentCache: Out of space for field %s (component %u, field %zu)",
					fieldName, compType, fieldIndex);
		return nullptr;
	}

	// Allocate from this field's zone
	size_t offsetInFieldZone = info.CurrentUsed;
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

void* TemporalComponentCache::GetFieldData(TemporalFrameHeader* header, ComponentTypeID compType,
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


bool TemporalComponentCache::TryLockFrameForWrite(uint32_t frameNum)
{
	TemporalFrameHeader* header = GetFrameHeader(frameNum);

	// Try to acquire write lock - frame must be completely unlocked (no readers or writers)
	// 0x01 = LOGIC_WRITING, 0x02 = RENDER_READING, 0x04 = NETWORK_READING, 0x08 = DEFRAG_LOCKED
	uint8_t expected = 0;
	return header->OwnershipFlags.compare_exchange_strong(expected, 0x01, std::memory_order_acquire);
}

bool TemporalComponentCache::VerifyFrameReadable(uint32_t frameNum) const
{
	const TemporalFrameHeader* header = GetFrameHeader(frameNum);

	// Frame is readable if no write lock is held (0x01 = LOGIC_WRITING)
	uint8_t flags = header->OwnershipFlags.load(std::memory_order_acquire);
	return (flags & 0x01) == 0;
}

void TemporalComponentCache::UnlockFrameWrite(uint32_t frameNum)
{
	TemporalFrameHeader* header = GetFrameHeader(frameNum);

	// Release write lock (clear LOGIC_WRITING bit)
	header->OwnershipFlags.fetch_and(~0x01, std::memory_order_release);
}

bool TemporalComponentCache::TryLockFrameForRead(uint32_t frameNum)
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

void TemporalComponentCache::UnlockFrameRead(uint32_t frameNum)
{
	TemporalFrameHeader* header = GetFrameHeader(frameNum);

	// Release write lock (clear LOGIC_WRITING bit)
	header->OwnershipFlags.fetch_and(~0x02, std::memory_order_release);
}

size_t TemporalComponentCache::AlignSize(size_t size)
{
	return (size + FIELD_ARRAY_ALIGNMENT - 1) & ~(FIELD_ARRAY_ALIGNMENT - 1);
}