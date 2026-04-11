#include "../Public/Archetype.h"
#include "Profiler.h"
#include "TemporalComponentCache.h"
#include "CacheSlotMeta.h"
#include <cassert>
#include <cstring>
#include <FieldMeta.h>
#include "ReflectionRegistry.h"

#include "Registry.h"

Archetype::Archetype(const Signature& sig, const ClassID& id, const char* debugName)
	: ArchSignature(sig)
	, ArchClassID(id)
	, DebugName(debugName)
{
}

Archetype::Archetype(const ArchetypeKey& archKey, const char* debugName)
	: ArchSignature(archKey.Sig)
	, ArchClassID(archKey.ID)
	, DebugName(debugName)
{
}

Archetype::~Archetype()
{
	// Clean up all allocated chunks
	for (Chunk* ChunkPtr : Chunks)
	{
		// Tracy memory profiling: Track chunk deallocation with pool name
		TNX_FREE_N(ChunkPtr, DebugName);
#ifdef _MSC_VER
		_aligned_free(ChunkPtr);
#else
		free(ChunkPtr);
#endif
	}
	Chunks.clear();
}

// Populates ArchetypeFieldLayout from the component list and computes TotalChunkDataSize.
// For each field: queries its CacheTier from ComponentFieldRegistry, then records a FieldDescriptor
// with the appropriate frame count and stride (cold fields get frameCount=1, frameStride=0).
// Called exactly once per archetype by Registry::GetOrCreateArchetype / InitializeArchetypes.
void Archetype::BuildLayout(Registry* reg, const std::vector<ComponentMetaEx>& components, SystemID inArchSystemID)
{
	assert(ArchetypeFieldLayout.count() == 0 && "BuildLayout called multiple times on same archetype");
	Reg          = reg;
	ArchSystemID = inArchSystemID;

	constexpr size_t ReservedHeaderSpace = Chunk::HEADER_SIZE;

	// Entity class declares chunk size via EntitiesPerChunk or inherits the default (256).
	EntitiesPerChunk = ReflectionRegistry::Get().EntityGetters[ArchClassID].EntitiesPerChunk;

	size_t currentOffset   = ReservedHeaderSpace;
	uint8_t ArchfieldIndex = 0;

	for (const ComponentMetaEx& comp : components)
	{
		ComponentTypeID typeID = comp.TypeID;

		const std::vector<FieldMeta>* fields    = ReflectionRegistry::Get().GetFields(typeID);
		const CacheTier temporalTier            = ReflectionRegistry::Get().GetTemporalTier(typeID);
		const uint8_t cacheSlotID               = ReflectionRegistry::Get().GetCacheSlotIndex(typeID);
		const ComponentCacheBase* temporalCache = reg->GetCache(temporalTier);

		if (!fields || fields->empty())
		{
			LOG_ERROR_F("Component %u has no fields registered", typeID);
			return;
		}

		LOG_INFO_F("Component %u: %zu fields", typeID, fields->size());

		for (uint8_t fieldIdx = 0; fieldIdx < fields->size(); ++fieldIdx)
		{
			const FieldMeta& field = (*fields)[fieldIdx];
			currentOffset          = AlignOffset(currentOffset, FIELD_ARRAY_ALIGNMENT);
			currentOffset          += field.Size * EntitiesPerChunk;

			FieldKey key{typeID, cacheSlotID, static_cast<uint32_t>(fieldIdx)};
			ArchetypeFieldLayout.insert_or_assign(key, FieldDescriptor{
													  ArchfieldIndex++,
													  fieldIdx,
													  cacheSlotID,
													  typeID,
													  temporalTier,
													  field.ValueType,
													  field.RefAssetType,
													  field.Size,
													  temporalCache ? temporalCache->GetTotalFrameCount() : 1, // 1 frame for cold — makes frame % 1 == 0, no branch needed
													  temporalCache ? temporalCache->GetFrameStride() : 0,
													  temporalCache ? true : false
												  });
		}
	}

	static constexpr size_t MinEntitySize = 64;
	TotalChunkDataSize                    = std::max(currentOffset, MinEntitySize);

	LOG_INFO_F("Archetype layout: %zu field arrays, %zu bytes, %u entities/chunk",
			   ArchetypeFieldLayout.count(), TotalChunkDataSize, EntitiesPerChunk);

	// Validate temporal field count doesn't exceed chunk header capacity
	assert(ArchetypeFieldLayout.count() <= Chunk::MAX_CHUNK_FIELDS);
}

// Returns the number of allocated slots in a specific chunk (includes tombstoned).
// Use for iteration bounds — callers rely on bitplane/masked-store to skip dead slots.
uint32_t Archetype::GetAllocatedChunkCount(size_t chunkIndex) const
{
	if (Chunks.empty() || chunkIndex >= Chunks.size() || EntitiesPerChunk == 0) return 0;

	if (chunkIndex == Chunks.size() - 1)
	{
		uint32_t remainder = AllocatedEntityCount % EntitiesPerChunk;
		return (remainder == 0 && AllocatedEntityCount > 0) ? EntitiesPerChunk : remainder;
	}

	return EntitiesPerChunk;
}

// Returns the number of live (non-tombstoned) entities in a specific chunk.
// TODO: Currently an approximation — derives per-chunk count from a global TotalEntityCount
// counter, which doesn't track which chunks the removals came from. Needs per-chunk live
// counters or Active flag scanning to be accurate.
uint32_t Archetype::GetLiveChunkCount(size_t chunkIndex) const
{
	if (Chunks.empty() || chunkIndex >= Chunks.size() || EntitiesPerChunk == 0) return 0;

	if (chunkIndex == Chunks.size() - 1)
	{
		uint32_t remainder = TotalEntityCount % EntitiesPerChunk;
		return (remainder == 0 && TotalEntityCount > 0) ? EntitiesPerChunk : remainder;
	}

	return EntitiesPerChunk;
}

// Allocates entity slots, filling outSlots with chunk/index/cache information.
// Prefers reusing InactiveEntitySlots (from prior RemoveEntity calls) before
// appending to the tail. Allocates new chunks as needed.
void Archetype::PushEntities(std::span<EntitySlot> outSlots)
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);

	assert(EntitiesPerChunk > 0 && "EntitiesPerChunk must be set by BuildLayout before pushing entities");
	const size_t count = outSlots.size();

	// Ensure enough chunks exist for new fresh allocations (worst case: no inactive slots to reuse)
	while ((AllocatedEntityCount + count) / EntitiesPerChunk >= Chunks.size())
	{
		Chunk* NewChunk = AllocateChunk();
		Chunks.push_back(NewChunk);
		ActiveEntitySlots.resize(Chunks.size() * EntitiesPerChunk);
	}

	for (auto& Slot : outSlots)
	{
		if (!InactiveEntitySlots.empty())
		{
			// Reuse a previously tombstoned slot — already counted in AllocatedEntityCount
			Slot = InactiveEntitySlots.back();
			InactiveEntitySlots.pop_back();
		}
		else
		{
			// Fresh allocation at the high-water mark
			uint32_t ChunkIndex = AllocatedEntityCount / EntitiesPerChunk;
			uint32_t LocalIndex = AllocatedEntityCount % EntitiesPerChunk;

			Slot.TargetChunk = Chunks[ChunkIndex];
			Slot.ChunkIndex  = ChunkIndex;
			Slot.LocalIndex  = LocalIndex;
			Slot.ArchIndex   = AllocatedEntityCount;
			Slot.CacheIndex  = Chunks[ChunkIndex]->Header.CacheIndexStart + LocalIndex;

			AllocatedEntityCount++;
		}

		ActiveEntitySlots[Slot.ArchIndex] = Slot;
		TotalEntityCount++;
	}
}

// Tombstones an entity by clearing its Active and Alive flags and setting Dirty in the Flags field,
// then moves the slot from ActiveEntitySlots to InactiveEntitySlots for future reuse.
// The Dirty flag propagates through the GPU pipeline: the predicate shader reads it and
// excludes the entity from draw commands. Memory is not freed here — the slot stays
// allocated in the chunk and can be reclaimed by PushEntities.
void Archetype::RemoveEntity(size_t chunkIndex, uint32_t localIndex, uint32_t archetypeIdx)
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);

	if (AllocatedEntityCount == 0) return;
	if (chunkIndex >= Chunks.size()) return;
	if (localIndex >= EntitiesPerChunk) return;

	// Write Dirty into the Flags field so the GPU predicate shader stops drawing this entity.
	{
		Chunk* chunk                = Chunks[chunkIndex];
		ComponentTypeID flagsTypeID = CacheSlotMeta<>::StaticTypeID();
		FieldKey flagKey{flagsTypeID, ReflectionRegistry::Get().GetCacheSlotIndex(flagsTypeID), 0};
		auto* flagDesc = ArchetypeFieldLayout.find(flagKey);
		assert(flagDesc && "Flags field missing from archetype layout");

		auto* flagsBase = static_cast<uint8_t*>(chunk->GetFieldPtr(flagDesc->fieldSlotIndex));

		// Offset into the correct write frame for temporal/volatile fields
		if (flagDesc->bIsTemporal)
		{
			uint32_t writeFrame = Reg->GetCache(flagDesc->tier)->GetActiveWriteFrame();
			flagsBase           += writeFrame * flagDesc->fieldFrameStride;
		}

		auto* metaInfo = reinterpret_cast<uint32_t*>(flagsBase) + localIndex;
		*metaInfo      = static_cast<uint32_t>(TemporalFlagBits::Dirty);
	}

	InactiveEntitySlots.push_back(ActiveEntitySlots[archetypeIdx]);

	// Only TotalEntityCount is decremented (live count).
	// AllocatedEntityCount stays — it's the high-water mark for iteration bounds.
	TotalEntityCount--;
}

// Returns base pointers (frame 0) for every field of a given component type within a chunk.
// Used by external code that needs per-field access without going through BuildFieldArrayTable.
std::vector<void*> Archetype::GetFieldArrays(Chunk* targetChunk, ComponentTypeID typeID)
{
	auto& cfr                            = ReflectionRegistry::Get();
	const std::vector<FieldMeta>* fields = cfr.GetFields(typeID);

	if (!fields || fields->empty())
	{
		LOG_ERROR_F("Component %u has no fields registered", typeID);
		return {};
	}

	std::vector<void*> fieldArrays;
	fieldArrays.reserve(fields->size());

	for (size_t fieldIdx = 0; fieldIdx < fields->size(); ++fieldIdx)
	{
		FieldKey key{typeID, cfr.GetCacheSlotIndex(typeID), static_cast<uint32_t>(fieldIdx)};
		auto it = ArchetypeFieldLayout.find(key);
		if (it)
		{
			fieldArrays.push_back(targetChunk->GetFieldPtr(it->fieldSlotIndex));
		}
	}

	return fieldArrays;
}

// Allocates a 64-byte aligned chunk, wires FieldPtrs[] for every field in the layout,
// and advances all cache allocators to keep entityCacheIDs globally synchronized.
// Includes Tracy fragmentation diagnostics (gap tracking, span efficiency).
Chunk* Archetype::AllocateChunk()
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);

#ifdef _MSC_VER
	auto NewChunk = static_cast<Chunk*>(_aligned_malloc(TotalChunkDataSize, 64));
#else
	auto NewChunk = static_cast<Chunk*>(aligned_alloc(64, TotalChunkDataSize));
#endif

	// Tracy memory profiling: Track chunk allocation with pool name
	// This lets you see separate pools for Archetypes
	TNX_ALLOC_N(NewChunk, TotalChunkDataSize, DebugName);

	NewChunk->Header            = Chunk::ChunkHeader{};
	NewChunk->Header.FieldCount = static_cast<uint8_t>(ArchetypeFieldLayout.count());

	size_t largestSize   = 0;
	size_t currentOffset = Chunk::HEADER_SIZE;
	auto* chunkBase      = reinterpret_cast<uint8_t*>(NewChunk);

	// Wire each field's FieldPtrs[] entry:
	//   Temporal/Volatile → allocated in the slab (pointer into ring buffer)
	//   Cold              → packed inline after the chunk header
	for (const auto& [fkey, fdesc] : ArchetypeFieldLayout)
	{
		largestSize = std::max(largestSize, fdesc.fieldSize);
		if (fdesc.bIsTemporal)
		{
			ComponentCacheBase* TemporalCache                = Reg->GetCache(fdesc.tier);
			NewChunk->Header.FieldPtrs[fdesc.fieldSlotIndex] = TemporalCache->AllocateFieldArray(
				this,
				NewChunk,
				fdesc.temporalComponentIndex,
				fdesc.componentSlotIndex,
				"",
				EntitiesPerChunk,
				fdesc.fieldSize,
				ArchSystemID);
		}
		else
		{
			currentOffset                                    = AlignOffset(currentOffset, FIELD_ARRAY_ALIGNMENT);
			NewChunk->Header.FieldPtrs[fdesc.fieldSlotIndex] = chunkBase + currentOffset;
			currentOffset                                    += fdesc.fieldSize * EntitiesPerChunk;
		}
	}

	// Advance ALL caches so entityCacheIDs stay globally synchronized.
	// An archetype may only store fields in one cache, but the allocator index
	// must advance in every cache so that entityCacheID N refers to the same
	// entity slot regardless of which cache you look at.
	NewChunk->Header.CacheIndexStart = Reg->GetVolatileCache()->AdvanceAllocator(ArchSystemID, EntitiesPerChunk, largestSize);
#ifdef TNX_ENABLE_ROLLBACK
	Reg->GetTemporalCache()->AdvanceAllocator(ArchSystemID, EntitiesPerChunk, largestSize);
#endif

	LOG_INFO_F("Allocated chunk with %i entities at cache index %zi", EntitiesPerChunk, NewChunk->Header.CacheIndexStart);

	// Debug: Track virtual memory fragmentation
	// This helps answer: "Why is 'spanned' so much larger than 'used'?"
	static void* lastChunk     = nullptr;
	static void* firstChunk    = nullptr;
	static uint32_t chunkCount = 0;

	if (firstChunk == nullptr)
	{
		firstChunk = NewChunk;
	}

	if (lastChunk != nullptr)
	{
		ptrdiff_t gap = (char*)NewChunk - static_cast<char*>(lastChunk);
		TNX_PLOT("Chunk Gap (KB)", gap / 1024.0);

		// Log suspicious gaps (> 100KB means something's between chunks)
		if (gap > 100 * 1024)
		{
			char buffer[256];
			snprintf(buffer, sizeof(buffer), "Large gap detected: %ti KB between chunk %u and %u",
					 gap / 1024, chunkCount - 1, chunkCount);
			TNX_ZONE_TEXT(buffer, strlen(buffer));
		}
	}

	chunkCount++;

	// Track total span
	[[maybe_unused]] ptrdiff_t totalSpan = (char*)NewChunk - static_cast<char*>(firstChunk);
	TNX_PLOT("Total Span (MB)", totalSpan / (1024.0 * 1024.0));
	TNX_PLOT("Chunk Count", static_cast<int64_t>(chunkCount));
	TNX_PLOT("Efficiency %", (chunkCount * sizeof(Chunk) * 100.0) / (totalSpan > 0 ? totalSpan : 1));

	lastChunk = NewChunk;

	return NewChunk;
}

// Hard reset — frees all chunk memory and clears slot tracking.
// Used by Registry::ResetRegistry. After this, new entities go through AllocateChunk
// which re-wires slab field arrays at correct allocator offsets.
void Archetype::FreeAllChunks()
{
	for (Chunk* ChunkPtr : Chunks)
	{
		TNX_FREE_N(ChunkPtr, DebugName);
#ifdef _MSC_VER
		_aligned_free(ChunkPtr);
#else
		free(ChunkPtr);
#endif
	}
	Chunks.clear();
	ActiveEntitySlots.clear();
	InactiveEntitySlots.clear();
	AllocatedEntityCount = 0;
	TotalEntityCount     = 0;
}
