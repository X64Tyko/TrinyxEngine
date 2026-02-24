#include "../Public/Archetype.h"
#include "Profiler.h"
#include "TemporalComponentCache.h"
#include <cassert>
#include <FieldMeta.h>

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
        STRIGID_FREE_N(ChunkPtr, DebugName);
#ifdef _MSC_VER
        _aligned_free(ChunkPtr);
#else
        free(ChunkPtr);
#endif
    }
    Chunks.clear();
}

void Archetype::BuildLayout(const std::vector<ComponentMetaEx>& Components, TemporalComponentCache* temporalCache)
{
    // BuildLayout should only be called once
    assert(TemporalCache == nullptr && "BuildLayout called multiple times on same archetype");
    assert(TotalFieldArrayCount == 0 && "BuildLayout called multiple times on same archetype");

    // Store temporal cache pointer for use during chunk allocation
    TemporalCache = temporalCache;

    if (Components.empty())
    {
        // Empty archetype - set a reasonable default capacity
        // (Useful for entities with only script component, no data components)
        constexpr size_t ReservedHeaderSpace = Chunk::HEADER_SIZE;
        size_t UsableSpace = Chunk::DATA_SIZE - ReservedHeaderSpace;
        EntitiesPerChunk = static_cast<uint32_t>(UsableSpace / 64); // Assume 64 bytes per entity minimum
        return;
    }

    // Calculate total stride (sum of all component sizes, excluding temporal)
    size_t TotalStride = 0;

    for (const ComponentMetaEx& Meta : Components)
    {
        if (!Meta.IsTemporal)
        {
            TotalStride += Meta.Size;
        }
    }

    // Calculate how many entities fit in a chunk
    constexpr size_t ReservedHeaderSpace = Chunk::HEADER_SIZE;
    size_t UsableSpace = Chunk::DATA_SIZE - ReservedHeaderSpace;

    if (TotalStride > 0)
    {
        EntitiesPerChunk = static_cast<uint32_t>(UsableSpace / TotalStride);
    }
    else
    {
        EntitiesPerChunk = static_cast<uint32_t>(UsableSpace / 64); // Minimum 64 bytes per entity
    }

    size_t currentOffset = ReservedHeaderSpace;
    uint8_t temporalFieldIndex = 0; // Counter for temporal field array indices

    for (const auto& comp : Components)
    {
        ComponentTypeID typeID = comp.TypeID;

        // Check if component has pre-registered field decomposition
        const std::vector<FieldMeta>* fields =
            ComponentFieldRegistry::Get().GetFields(typeID);

        if (fields && !fields->empty())
        {
            // Component is decomposed - allocate separate field arrays
            LOG_INFO_F("Decomposing component %u into %zu field arrays",
                       typeID, fields->size());

            for (size_t fieldIdx = 0; fieldIdx < fields->size(); ++fieldIdx)
            {
                const FieldMeta& field = (*fields)[fieldIdx];

                // Check if this is a temporal component
                bool isTemporal = comp.IsTemporal;

                if (isTemporal && temporalCache)
                {
                    // Mark this field as temporal with array index - will be allocated per-chunk
                    FieldKey key{typeID, static_cast<uint32_t>(fieldIdx)};
                    if (TemporalFieldIndices.find(key) == TemporalFieldIndices.end())
                    {
                        TemporalFieldIndices[key] = temporalFieldIndex++;

                        LOG_INFO_F("  Temporal field %s[%zu] will be allocated per-chunk (index %u)",
                                   field.Name, fieldIdx, temporalFieldIndex - 1);
                    }
                }
                else
                {
                    // Regular chunk allocation
                    // Align offset for this field array
                    currentOffset = AlignOffset(currentOffset, field.Alignment);

                    // Store offset for this field array
                    FieldKey key{typeID, static_cast<uint32_t>(fieldIdx)};
                    FieldOffsets[key] = currentOffset;

                    LOG_TRACE_F("  Field %s[%zu]: offset=%zu, size=%zu",
                                field.Name, fieldIdx, currentOffset, field.Size);

                    // Advance by EntitiesPerChunk * field size
                    currentOffset += EntitiesPerChunk * field.Size;
                }

                // Add to cached layout
                CachedFieldArrayLayout.push_back({
                    typeID,
                    static_cast<uint32_t>(fieldIdx),
                    true
                });

                // Add to template cache
                FieldArrayTemplateCache.push_back({
                    currentOffset,
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
                comp.Size,
                comp.Alignment,
                currentOffset
            };

            // Add to cached layout as single array
            CachedFieldArrayLayout.push_back({
                typeID,
                0,
                false
            });

            // Add to template cache
            FieldArrayTemplateCache.push_back({
                currentOffset,
                "non_decomposed"
            });

            currentOffset += EntitiesPerChunk * comp.Size;
            TotalFieldArrayCount += 1;
        }
    }

    TotalChunkDataSize = currentOffset;

    // Cache frame stride if we have temporal fields
    if (!TemporalFieldIndices.empty() && TemporalCache)
    {
        TemporalFrameStride = TemporalCache->GetFrameStride();
    }

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
    if (Chunks.empty() || ChunkIndex >= Chunks.size() || EntitiesPerChunk == 0)
        return 0;

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

Archetype::EntitySlot Archetype::PushEntity()
{
    STRIGID_ZONE_C(STRIGID_COLOR_MEMORY);
    // Safety check for empty archetypes
    if (EntitiesPerChunk == 0)
    {
        EntitiesPerChunk = 256; // Default fallback
    }

    // Check if we need a new chunk
    if (TotalEntityCount / EntitiesPerChunk >= Chunks.size())
    {
        Chunk* NewChunk = AllocateChunk();
        Chunks.push_back(NewChunk);
    }

    // Calculate which chunk and local index
    uint32_t ChunkIndex = TotalEntityCount / EntitiesPerChunk;
    uint32_t LocalIndex = TotalEntityCount % EntitiesPerChunk;

    EntitySlot Slot;
    Slot.TargetChunk = Chunks[ChunkIndex];
    Slot.LocalIndex = LocalIndex;
    Slot.GlobalIndex = TotalEntityCount;

    TotalEntityCount++;

    return Slot;
}

void Archetype::RemoveEntity(size_t ChunkIndex, uint32_t LocalIndex)
{
    STRIGID_ZONE_C(STRIGID_COLOR_MEMORY);

    if (TotalEntityCount == 0)
        return;

    // Calculate the last entity's location
    uint32_t LastChunkIndex = (TotalEntityCount - 1) / EntitiesPerChunk;
    uint32_t LastLocalIndex = (TotalEntityCount - 1) % EntitiesPerChunk;

    // If we're not removing the last entity, swap it with the last
    if (ChunkIndex != LastChunkIndex || LocalIndex != LastLocalIndex)
    {
        Chunk* TargetChunk = Chunks[ChunkIndex];
        Chunk* LastChunk = Chunks[LastChunkIndex];

        // Swap all regular field arrays (non-temporal)
        for (const auto& [key, offset] : FieldOffsets)
        {
            const std::vector<FieldMeta>* fields = ComponentFieldRegistry::Get().GetFields(key.componentID);
            if (!fields || key.fieldIndex >= fields->size())
                continue;

            const FieldMeta& field = (*fields)[key.fieldIndex];

            void* targetFieldArray = TargetChunk->GetBuffer(static_cast<uint32_t>(offset));
            void* lastFieldArray = LastChunk->GetBuffer(static_cast<uint32_t>(offset));

            // Swap using field size
            char* target = static_cast<char*>(targetFieldArray) + (LocalIndex * field.Size);
            char* last = static_cast<char*>(lastFieldArray) + (LastLocalIndex * field.Size);

            memcpy(target, last, field.Size);
        }

        // Swap temporal field arrays (all frames)
        if (TemporalCache && !TemporalFieldIndices.empty())
        {
            ComponentFieldRegistry& CFR = ComponentFieldRegistry::Get();

            for (const auto& [key, arrayIndex] : TemporalFieldIndices)
            {
                const std::vector<FieldMeta>* fields = CFR.GetFields(key.componentID);
                if (!fields || key.fieldIndex >= fields->size())
                    continue;

                const FieldMeta& field = (*fields)[key.fieldIndex];

                // Get frame 0 pointers for both chunks
                uint8_t* targetFrame0 = static_cast<uint8_t*>(TargetChunk->GetTemporalFieldPointer(arrayIndex));
                uint8_t* lastFrame0 = static_cast<uint8_t*>(LastChunk->GetTemporalFieldPointer(arrayIndex));

                // Swap across all temporal frames
                size_t frameCount = TemporalCache->GetTotalFrameCount();
                for (size_t frameIdx = 0; frameIdx < frameCount; ++frameIdx)
                {
                    size_t frameOffset = frameIdx * TemporalFrameStride;

                    char* target = reinterpret_cast<char*>(targetFrame0 + frameOffset) + (LocalIndex * field.Size);
                    char* last = reinterpret_cast<char*>(lastFrame0 + frameOffset) + (LastLocalIndex * field.Size);

                    memcpy(target, last, field.Size);
                }
            }
        }

        // Also handle non-decomposed components
        for (const auto& [typeID, meta] : ComponentLayout)
        {
            void* targetArray = TargetChunk->GetBuffer(static_cast<uint32_t>(meta.OffsetInChunk));
            void* lastArray = LastChunk->GetBuffer(static_cast<uint32_t>(meta.OffsetInChunk));

            char* target = static_cast<char*>(targetArray) + (LocalIndex * meta.Size);
            char* last = static_cast<char*>(lastArray) + (LastLocalIndex * meta.Size);

            memcpy(target, last, meta.Size);
        }
    }

    // Decrement entity count
    TotalEntityCount--;

    // If the last chunk is now empty, we could deallocate it
    // (optional optimization - for now we keep it allocated)
}

std::vector<void*> Archetype::GetFieldArrays(Chunk* TargetChunk, ComponentTypeID TypeID)
{
    // Check if component is decomposed
    const std::vector<FieldMeta>* fields = ComponentFieldRegistry::Get().GetFields(TypeID);

    if (fields && !fields->empty())
    {
        // Decomposed component - return all field arrays
        std::vector<void*> fieldArrays;
        fieldArrays.reserve(fields->size());

        for (size_t fieldIdx = 0; fieldIdx < fields->size(); ++fieldIdx)
        {
            FieldKey key{TypeID, static_cast<uint32_t>(fieldIdx)};
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
    if (It == ComponentLayout.end())
        return {};

    const ComponentMetaEx& Meta = It->second;
    return {TargetChunk->GetBuffer(static_cast<uint32_t>(Meta.OffsetInChunk))};
}

Chunk* Archetype::AllocateChunk()
{
    STRIGID_ZONE_C(STRIGID_COLOR_MEMORY);
#ifdef _MSC_VER
    auto NewChunk = static_cast<Chunk*>(_aligned_malloc(sizeof(Chunk), 64));
#else
    auto NewChunk = static_cast<Chunk*>(aligned_alloc(64, sizeof(Chunk)));
#endif

    // Tracy memory profiling: Track chunk allocation with pool name
    // This lets you see separate pools for Archetypes
    STRIGID_ALLOC_N(NewChunk, sizeof(Chunk), DebugName);

    // Allocate temporal field arrays for this chunk
    if (TemporalCache && !TemporalFieldIndices.empty())
    {
        ComponentFieldRegistry& CFR = ComponentFieldRegistry::Get();

        for (const auto& [key, arrayIndex] : TemporalFieldIndices)
        {
            const std::vector<FieldMeta>* fields = CFR.GetFields(key.componentID);
            if (!fields || key.fieldIndex >= fields->size())
                continue;

            const FieldMeta& field = (*fields)[key.fieldIndex];

            void* temporalPtr = TemporalCache->AllocateFieldArray(
                this,
                NewChunk,
                key.componentID,
                key.fieldIndex,
                field.Name,
                EntitiesPerChunk,
                field.Size
            );

            // Store frame 0 pointer in chunk header at the assigned array index
            NewChunk->SetTemporalFieldPointer(arrayIndex, temporalPtr);
        }

        LOG_TRACE_F("Allocated %zu temporal field arrays for chunk", TemporalFieldIndices.size());
    }

    // Debug: Track virtual memory fragmentation
    // This helps answer: "Why is 'spanned' so much larger than 'used'?"
    static void* lastChunk = nullptr;
    static void* firstChunk = nullptr;
    static uint32_t chunkCount = 0;

    if (firstChunk == nullptr)
    {
        firstChunk = NewChunk;
    }

    if (lastChunk != nullptr)
    {
        ptrdiff_t gap = (char*)NewChunk - static_cast<char*>(lastChunk);
        STRIGID_PLOT("Chunk Gap (KB)", gap / 1024.0);

        // Log suspicious gaps (> 100KB means something's between chunks)
        if (gap > 100 * 1024)
        {
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "Large gap detected: %lld KB between chunk %u and %u",
                     gap / 1024, chunkCount - 1, chunkCount);
            STRIGID_ZONE_TEXT(buffer, strlen(buffer));
        }
    }

    chunkCount++;

    // Track total span
    ptrdiff_t totalSpan = (char*)NewChunk - static_cast<char*>(firstChunk);
    STRIGID_PLOT("Total Span (MB)", totalSpan / (1024.0 * 1024.0));
    STRIGID_PLOT("Chunk Count", static_cast<int64_t>(chunkCount));
    STRIGID_PLOT("Efficiency %", (chunkCount * sizeof(Chunk) * 100.0) / (totalSpan > 0 ? totalSpan : 1));

    lastChunk = NewChunk;

    return NewChunk;
}
