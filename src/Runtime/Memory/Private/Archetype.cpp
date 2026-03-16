#include "../Public/Archetype.h"
#include "Profiler.h"
#include "TemporalComponentCache.h"
#include "TemporalFlags.h"
#include <cassert>
#include <cstring>
#include <FieldMeta.h>

#include "Registry.h"

Archetype::Archetype(const Signature& Sig, const ClassID& ID, const char* DebugName)
	: ArchSignature(Sig)
	, ArchClassID(ID)
	, DebugName(DebugName)
{
}

Archetype::Archetype(const ArchetypeKey& ArchKey, const char* DebugName)
	: ArchSignature(ArchKey.Sig)
	, ArchClassID(ArchKey.ID)
	, DebugName(DebugName)
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

void Archetype::BuildLayout(Registry* reg, const std::vector<ComponentMetaEx>& Components, SystemID inArchSystemID)
{
	// BuildLayout should only be called once
	assert(TotalFieldArrayCount == 0 && "BuildLayout called multiple times on same archetype");
	Reg          = reg;
	ArchSystemID = inArchSystemID;

	// Calculate total stride (sum of all component sizes, excluding temporal)
	size_t TotalStride = 0;

	for (const ComponentMetaEx& Meta : Components)
	{
		if (Meta.TemporalTier == CacheTier::None)
		{
			TotalStride += Meta.Size;
		}
	}

	// Calculate how many entities fit in a chunk
	constexpr size_t ReservedHeaderSpace = Chunk::HEADER_SIZE;

	// Per-class override: entity declares static constexpr uint32_t kEntitiesPerChunk = N.
	// Must be applied before the layout loop so field offsets and TotalChunkDataSize use the new count.
	if (uint32_t override_ = MetaRegistry::Get().EntityGetters[ArchClassID].EntitiesPerChunk) EntitiesPerChunk = override_;

	size_t currentOffset       = ReservedHeaderSpace;
	uint8_t temporalFieldIndex = 0; // Counter for temporal field array indices
	uint8_t slotIdx            = 0;

	for (const auto& comp : Components)
	{
		ComponentTypeID typeID = comp.TypeID;

		// Check if component has pre-registered field decomposition
		const std::vector<FieldMeta>* fields =
			ComponentFieldRegistry::Get().GetFields(typeID);
		const CacheTier temporalTier            = ComponentFieldRegistry::Get().GetTemporalTier(typeID);
		const uint8_t cacheSlotID               = ComponentFieldRegistry::Get().GetCacheSlotIndex(typeID);
		const ComponentCacheBase* temporalCache = reg->GetCache(temporalTier);

		if (fields && !fields->empty())
		{
			// Component is decomposed - allocate separate field arrays
			LOG_INFO_F("Decomposing component %u into %zu field arrays",
					   typeID, fields->size());

			for (size_t fieldIdx = 0; fieldIdx < fields->size(); ++fieldIdx)
			{
				const FieldMeta& field = (*fields)[fieldIdx];

				size_t fieldFrames = 1;
				size_t frameStride = 0;
				size_t fieldOffset = currentOffset; // Track offset before advancing

				if (temporalTier != CacheTier::None)
				{
					// Mark this field as temporal with array index - will be allocated per-chunk
					FieldKey key{typeID, cacheSlotID, static_cast<uint32_t>(fieldIdx)};
					if (TemporalFieldIndices.find(key) == TemporalFieldIndices.end())
					{
						slotIdx                   = temporalFieldIndex;
						TemporalFieldIndices[key] = {
							temporalFieldIndex++,
							temporalCache->GetTotalFrameCount(),
							temporalCache->GetFrameStride()
						};

						LOG_INFO_F("  Temporal field %s[%zu] will be allocated per-chunk (index %u)",
								   field.Name, fieldIdx, temporalFieldIndex - 1);
					}

					fieldFrames = temporalCache->GetTotalFrameCount();
					frameStride = temporalCache->GetFrameStride();
				}
				else
				{
					slotIdx = fieldIdx;
					// Cold chunk allocation — single-frame SoA array in chunk data
					currentOffset = AlignOffset(currentOffset, field.Alignment);
					fieldOffset   = currentOffset;

					// Store offset for this field array
					FieldKey key{typeID, cacheSlotID, static_cast<uint32_t>(fieldIdx)};
					FieldOffsets[key] = currentOffset;

					LOG_TRACE_F("  Cold field %s[%zu]: offset=%zu, size=%zu",
								field.Name, fieldIdx, currentOffset, field.Size);

					// Advance by EntitiesPerChunk * field size
					currentOffset += EntitiesPerChunk * field.Size;
				}

				// Add to cached layout
				CachedFieldArrayLayout.push_back({
					cacheSlotID,
					static_cast<uint32_t>(fieldIdx),
					slotIdx,
					fieldFrames,
					frameStride,
					field.Size,
					true,
					temporalTier,
					field.ValueType
				});

				// Add to template cache (stores start offset, not end)
				FieldArrayTemplateCache.push_back({
					fieldOffset,
					field.Name
				});
			}

			TotalFieldArrayCount += fields->size();
		}
		else
		{
			// Non-decomposed component - store as single array
			LOG_INFO_F("Component %u stored as non-decomposed array", typeID);

			currentOffset = AlignOffset(currentOffset, comp.Alignment);

			ComponentLayout[typeID] = ComponentMetaEx{
				typeID,
				comp.Name,
				comp.Size,
				comp.Alignment,
				currentOffset,
				false,
				CacheTier::None,
				comp.CacheSlotIndex,
				std::vector<FieldMeta>()
			};

			// Add to cached layout as single array
			CachedFieldArrayLayout.push_back({
				typeID,
				0,
				1,
				0,
				comp.Size,
				false
			});

			// Add to template cache
			FieldArrayTemplateCache.push_back({
				currentOffset,
				"non_decomposed"
			});

			currentOffset        += EntitiesPerChunk * comp.Size;
			TotalFieldArrayCount += 1;
		}
	}

	static constexpr size_t MinEntitySize = 64;
	TotalChunkDataSize                    = std::max(currentOffset, MinEntitySize);

	LOG_INFO_F("Archetype layout: %zu field arrays (%zu temporal), %zu bytes, %u entities/chunk",
			   TotalFieldArrayCount, TemporalFieldIndices.size(), TotalChunkDataSize, EntitiesPerChunk);

	// Validate cache consistency
	assert(CachedFieldArrayLayout.size() == TotalFieldArrayCount);
	assert(FieldArrayTemplateCache.size() == TotalFieldArrayCount);

	// Validate temporal field count doesn't exceed chunk header capacity
	assert(TemporalFieldIndices.size() <= Chunk::MAX_TEMPORAL_FIELDS);
}

uint32_t Archetype::GetChunkCount(size_t ChunkIndex) const
{
	if (Chunks.empty() || ChunkIndex >= Chunks.size() || EntitiesPerChunk == 0) return 0;

	// If it's the last chunk, calculate remainder
	if (ChunkIndex == Chunks.size() - 1)
	{
		uint32_t Remainder = TotalEntityCount % EntitiesPerChunk;
		// Handle case where last chunk is exactly full
		return (Remainder == 0 && TotalEntityCount > 0) ? EntitiesPerChunk : Remainder;
	}

	// All other chunks are guaranteed full (dense packing invariant)
	return EntitiesPerChunk;
}

void Archetype::PushEntities(std::vector<EntitySlot>& outSlots, size_t count)
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);
	// Safety check for empty archetypes
	if (EntitiesPerChunk == 0)
	{
		EntitiesPerChunk = 256; // Default fallback
	}

	// Check if we need a new chunk
	while ((TotalEntityCount + count) / EntitiesPerChunk >= Chunks.size())
	{
		Chunk* NewChunk = AllocateChunk();
		Chunks.push_back(NewChunk);
		ActiveEntitySlots.resize(Chunks.size() * EntitiesPerChunk);
	}

	outSlots.reserve(count);
	for (auto& Slot : outSlots)
	{
		// Calculate which chunk and local index
		uint32_t ChunkIndex = TotalEntityCount / EntitiesPerChunk;
		uint32_t LocalIndex = TotalEntityCount % EntitiesPerChunk;

		if (!InactiveEntitySlots.empty())
		{
			Slot = InactiveEntitySlots.back();
			InactiveEntitySlots.pop_back();
		}
		else
		{
			Slot.TargetChunk = Chunks[ChunkIndex];
			Slot.LocalIndex  = LocalIndex;
			Slot.CacheIndex  = TotalEntityCount;
		}
		ActiveEntitySlots[Slot.CacheIndex] = Slot;

		TotalEntityCount++;
	}
}

void Archetype::RemoveEntity(size_t ChunkIndex, uint32_t LocalIndex, uint32_t ArchetypeIdx)
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);

	if (TotalEntityCount == 0) return;

	if constexpr (false)
	{
		// Calculate the last entity's location
		uint32_t LastChunkIndex = (TotalEntityCount - 1) / EntitiesPerChunk;
		uint32_t LastLocalIndex = (TotalEntityCount - 1) % EntitiesPerChunk;

		// If we're not removing the last entity, swap it with the last
		if (ChunkIndex != LastChunkIndex || LocalIndex != LastLocalIndex)
		{
			Chunk* TargetChunk = Chunks[ChunkIndex];
			Chunk* LastChunk   = Chunks[LastChunkIndex];

			// Swap all regular field arrays (non-temporal)
			for (const auto& [key, offset] : FieldOffsets)
			{
				const std::vector<FieldMeta>* fields = ComponentFieldRegistry::Get().GetFields(key.componentID);
				if (!fields || key.fieldIndex >= fields->size()) continue;

				const FieldMeta& field = (*fields)[key.fieldIndex];

				void* targetFieldArray = TargetChunk->GetBuffer(static_cast<uint32_t>(offset));
				void* lastFieldArray   = LastChunk->GetBuffer(static_cast<uint32_t>(offset));

				// Swap using field size
				char* target = static_cast<char*>(targetFieldArray) + (LocalIndex * field.Size);
				char* last   = static_cast<char*>(lastFieldArray) + (LastLocalIndex * field.Size);

				memcpy(target, last, field.Size);
			}

			size_t size = CachedFieldArrayLayout.size();
			for (size_t i = 0; i < size; ++i)
			{
				const auto& desc = CachedFieldArrayLayout[i];

				// Get frame 0 pointers for both chunks
				uint8_t* targetFrame0 = static_cast<uint8_t*>(TargetChunk->GetTemporalFieldPointer(desc.fieldIndex));
				uint8_t* lastFrame0   = static_cast<uint8_t*>(LastChunk->GetTemporalFieldPointer(desc.fieldIndex));

				// Swap across all temporal frames — use the cached scalar, no cache call needed.
				for (size_t frameIdx = 0; frameIdx < desc.FieldFrames; ++frameIdx)
				{
					size_t frameOffset = frameIdx * desc.FrameStride;

					char* target = reinterpret_cast<char*>(targetFrame0 + frameOffset) + (LocalIndex * desc.Size);
					char* last   = reinterpret_cast<char*>(lastFrame0 + frameOffset) + (LastLocalIndex * desc.Size);

					memcpy(target, last, desc.Size);
				}
			}

			// Also handle non-decomposed components
			for (const auto& [typeID, meta] : ComponentLayout)
			{
				void* targetArray = TargetChunk->GetBuffer(static_cast<uint32_t>(meta.OffsetInChunk));
				void* lastArray   = LastChunk->GetBuffer(static_cast<uint32_t>(meta.OffsetInChunk));

				char* target = static_cast<char*>(targetArray) + (LocalIndex * meta.Size);
				char* last   = static_cast<char*>(lastArray) + (LastLocalIndex * meta.Size);

				memcpy(target, last, meta.Size);
			}
		}
	}

	// Mark entity as inactive+dirty in the Flags field so the GPU predicate stops drawing it.
	// Find the TemporalFlags::Flags field in the layout and write to the current write frame.
	{
		Chunk* chunk            = Chunks[ChunkIndex];
		const uint8_t flagsSlot = ComponentFieldRegistry::Get().GetCacheSlotIndex(TemporalFlags<>::StaticTypeID());

		for (size_t i = 0; i < CachedFieldArrayLayout.size(); ++i)
		{
			const auto& desc = CachedFieldArrayLayout[i];
			if (desc.componentID != flagsSlot) continue;
			//if (desc.Tier == CacheTier::None) break; // Flags should always be temporal

			// Get the write frame pointer for the Flags field
			uint32_t writeFrame = Reg->GetTemporalCache()->GetActiveWriteFrame();
			auto* flagsBase     = static_cast<int32_t*>(
				static_cast<void*>(
					static_cast<uint8_t*>(chunk->GetTemporalFieldPointer(desc.SlotIndex))
					+ writeFrame * desc.FrameStride));

			// Clear Active, set Dirty so the renderer sees the removal
			flagsBase[LocalIndex] = static_cast<int32_t>(TemporalFlagBits::Dirty);

			// Also mark the entity dirty in the Registry's per-frame bitplane
			size_t cacheIdx     = chunk->Header.CacheIndexStart + LocalIndex;
			auto* dirtyBitplane = Reg->DirtyBitsFrame(writeFrame);
			uint64_t& word      = (*dirtyBitplane)[cacheIdx / 64];
			word                |= uint64_t(1) << (cacheIdx % 64);
			break;
		}
	}

	InactiveEntitySlots.push_back(ActiveEntitySlots[ArchetypeIdx]);

	// Decrement entity count
	TotalEntityCount--;

	// If the last chunk is now empty, we could deallocate it
	// (optional optimization - for now we keep it allocated)
}

std::vector<void*> Archetype::GetFieldArrays(Chunk* TargetChunk, ComponentTypeID TypeID)
{
	// Check if component is decomposed
	const std::vector<FieldMeta>* fields = ComponentFieldRegistry::Get().GetFields(TypeID);
	ComponentFieldRegistry& CFR          = ComponentFieldRegistry::Get();

	if (fields && !fields->empty())
	{
		// Decomposed component - return all field arrays
		std::vector<void*> fieldArrays;
		fieldArrays.reserve(fields->size());

		for (size_t fieldIdx = 0; fieldIdx < fields->size(); ++fieldIdx)
		{
			FieldKey key{TypeID, CFR.GetCacheSlotIndex(TypeID), static_cast<uint32_t>(fieldIdx)};
			auto it = FieldOffsets.find(key);
			if (it != FieldOffsets.end())
			{
				fieldArrays.push_back(TargetChunk->GetBuffer(static_cast<uint32_t>(it->second)));
			}
		}

		return fieldArrays;
	}
	// Non-decomposed component - return single array
	auto It = ComponentLayout.find(TypeID);
	if (It == ComponentLayout.end()) return {};

	const ComponentMetaEx& Meta = It->second;
	return {TargetChunk->GetBuffer(static_cast<uint32_t>(Meta.OffsetInChunk))};
}

Chunk* Archetype::AllocateChunk()
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);
	// TotalChunkDataSize = HEADER_SIZE + (EntitiesPerChunk * all field strides), set in BuildLayout.
	// This accounts for per-class kEntitiesPerChunk overrides which may exceed DATA_SIZE.
	size_t newChunkSize = TotalChunkDataSize * EntitiesPerChunk + Chunk::HEADER_SIZE;
#ifdef _MSC_VER
	auto NewChunk = static_cast<Chunk*>(_aligned_malloc(newChunkSize, 64));
#else
	auto NewChunk = static_cast<Chunk*>(aligned_alloc(64, newChunkSize));
#endif

	// Tracy memory profiling: Track chunk allocation with pool name
	// This lets you see separate pools for Archetypes
	TNX_ALLOC_N(NewChunk, newChunkSize, DebugName);

	std::unordered_set<ComponentCacheBase*> UsedCaches(3);
	size_t largestSize = 0;

	// Allocate temporal field arrays for this chunk
	if (!TemporalFieldIndices.empty())
	{
		ComponentFieldRegistry& CFR = ComponentFieldRegistry::Get();
		for (const auto& [key, fieldData] : TemporalFieldIndices)
		{
			const std::vector<FieldMeta>* fields = CFR.GetFields(key.componentID);
			const CacheTier temporalTier         = CFR.GetTemporalTier(key.componentID);
			ComponentCacheBase* TemporalCache    = Reg->GetCache(temporalTier);
			UsedCaches.insert(TemporalCache);

			if (!fields || key.fieldIndex >= fields->size()) continue;

			const FieldMeta& field = (*fields)[key.fieldIndex];
			largestSize            = std::max(largestSize, field.Size);

			void* temporalPtr = TemporalCache->AllocateFieldArray(
				this,
				NewChunk,
				key.temporalFieldIndex,
				key.fieldIndex,
				field.Name,
				EntitiesPerChunk,
				field.Size,
				ArchSystemID
			);

			// Store frame 0 pointer in chunk header at the assigned array index
			NewChunk->SetTemporalFieldPointer(fieldData.SlotIndex, temporalPtr);
		}

		LOG_TRACE_F("Allocated %zu temporal field arrays for chunk", TemporalFieldIndices.size());
	}

	for (auto& cache : UsedCaches) NewChunk->Header.CacheIndexStart = cache->AdvanceAllocator(ArchSystemID, EntitiesPerChunk, largestSize);

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
	TotalEntityCount = 0;
}
