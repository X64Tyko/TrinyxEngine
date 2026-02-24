#pragma once
#include "Types.h"
#include "Signature.h"
#include "Chunk.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "FieldMeta.h"
#include "Schema.h"

// Archetype - manages storage for entities with a specific component signature
// Uses Structure-of-Arrays (SoA) layout within each chunk
class Archetype
{
public:
    struct ArchetypeKey
    {
        Signature Sig;
        ClassID ID;

        bool operator==(const ArchetypeKey& other) const
        {
            return ID == other.ID && Sig == other.Sig;
        }
    };

    Archetype(const Signature& Sig, const ClassID& ID, const char* DebugName = "Archetype");
    Archetype(const ArchetypeKey& ArchKey, const char* DebugName = "Archetype");
    ~Archetype();

    // Component signature
    Signature ArchSignature;

    // ClassID - needed for using the correct entity during Hydration
    ClassID ArchClassID;

    // Debug name for profiling
    const char* DebugName;

    // Entity capacity and tracking
    uint32_t EntitiesPerChunk = 0; // How many entities fit in one chunk
    uint32_t TotalEntityCount = 0; // Total entities across all chunks

    // Chunk storage
    std::vector<Chunk*> Chunks;

    // Component layout information
    std::unordered_map<ComponentTypeID, ComponentMetaEx> ComponentLayout;

    // Cached component iteration data (built once in BuildLayout)
    struct ComponentCacheEntry
    {
        ComponentTypeID TypeID;
        bool IsFieldDecomposed;
        size_t ChunkOffset;
    };

    std::vector<ComponentCacheEntry> ComponentIterationCache;

    // Get the number of entities in a specific chunk (handles tail chunk)
    uint32_t GetChunkCount(size_t ChunkIndex) const;

    // Allocate a new entity slot (returns chunk and local index)
    struct EntitySlot
    {
        Chunk* TargetChunk;
        uint32_t LocalIndex;
        uint32_t GlobalIndex; // Index across all chunks
    };

    EntitySlot PushEntity();

    // Remove an entity (swap-and-pop, deferred via active mask)
    void RemoveEntity(size_t ChunkIndex, uint32_t LocalIndex);

    // Get typed array pointer for a component in a specific chunk
    template <typename T>
    T* GetComponentArray(Chunk* TargetChunk, ComponentTypeID TypeID)
    {
        auto It = ComponentLayout.find(TypeID);
        if (It == ComponentLayout.end())
            return nullptr;

        const ComponentMetaEx& Meta = It->second;
        return reinterpret_cast<T*>(TargetChunk->GetBuffer(static_cast<uint32_t>(Meta.OffsetInChunk)));
    }

    template <typename T>
    T* GetComponent(Chunk* TargetChunk, ComponentTypeID TypeID, uint32_t Index)
    {
        // TODO: need to verify this Index is valid
        return GetComponentArray<T>(TargetChunk, TypeID)[Index];
    }

    // Get field arrays for decomposed components (SoA)
    std::vector<void*> GetFieldArrays(Chunk* TargetChunk, ComponentTypeID TypeID);

    // Build the internal SoA layout from component list
    void BuildLayout(const std::vector<ComponentMetaEx>& Components, class TemporalComponentCache* temporalCache = nullptr);

    // Edge graph for archetype transitions (future optimization)
    std::unordered_map<ComponentTypeID, Archetype*> AddEdges; // Add component X -> go to archetype Y
    std::unordered_map<ComponentTypeID, Archetype*> RemoveEdges; // Remove component X -> go to archetype Y

    // Field array lookup key
    struct FieldKey
    {
        ComponentTypeID componentID;
        uint32_t fieldIndex;

        bool operator==(const FieldKey& other) const
        {
            return componentID == other.componentID && fieldIndex == other.fieldIndex;
        }
    };

    struct FieldKeyHash
    {
        size_t operator()(const FieldKey& key) const
        {
            return (static_cast<size_t>(key.componentID) << 32) | key.fieldIndex;
        }
    };

    // Storage for field array offsets
    std::unordered_map<FieldKey, size_t, FieldKeyHash> FieldOffsets;

    // CACHED FIELD ARRAY TABLE - computed once after BuildLayout()
    // This stores the field array count and layout order
    struct FieldArrayDescriptor
    {
        ComponentTypeID componentID;
        uint32_t fieldIndex;
        bool isDecomposed;
    };

    std::vector<FieldArrayDescriptor> CachedFieldArrayLayout;
    size_t TotalFieldArrayCount = 0;

    // Pre-compute field array offsets (chunk-independent)
    // Call this once after BuildLayout() to cache offsets
    struct FieldArrayTemplate
    {
        size_t offsetInChunk;
        const char* debugName; // For debugging
    };

    std::vector<FieldArrayTemplate> FieldArrayTemplateCache;

    size_t TotalChunkDataSize = 0;

    // Track which fields are temporal with array index mapping
    // Key: (ComponentTypeID, fieldIndex) → chunk header array index
    std::unordered_map<FieldKey, uint8_t, FieldKeyHash> TemporalFieldIndices;

    // Temporal cache pointer (needed during chunk allocation)
    class TemporalComponentCache* TemporalCache = nullptr;

    // Cached frame stride (0 if no temporal fields)
    size_t TemporalFrameStride = 0;

    // Get pointer to a specific field array within a chunk (always returns frame 0 for temporal)
    void* GetFieldArray(Chunk* chunk, ComponentTypeID typeID, uint32_t fieldIndex)
    {
        FieldKey key{typeID, fieldIndex};

        // Check if this is a temporal field
        auto temporalIt = TemporalFieldIndices.find(key);
        if (temporalIt != TemporalFieldIndices.end())
        {
            // Return frame 0 pointer from chunk header
            return chunk->GetTemporalFieldPointer(temporalIt->second);
        }

        // Regular chunk field
        auto it = FieldOffsets.find(key);
        if (it == FieldOffsets.end()) return nullptr;

        return chunk->GetBuffer(static_cast<uint32_t>(it->second));
    }

    // Build interleaved dual field array table (read T, write T+1) for FieldProxy::Bind()
    // Output layout: [read0, write0, read1, write1, read2, write2, ...]
    // currentFrame: frame T (read from T, write to T+1)
    // writeFrame: frame T+1 (with wrapping handled by caller)
    void BuildFieldArrayTable(Chunk* chunk, void** outDualArrayTable, uint32_t currentFrame, uint32_t writeFrame)
    {
        auto chunkBase = chunk->Data;

        size_t frameOffset_T = currentFrame * TemporalFrameStride;
        size_t frameOffset_T1 = writeFrame * TemporalFrameStride;

        size_t size = CachedFieldArrayLayout.size();
        for (size_t i = 0; i < size; ++i)
        {
            const auto& desc = CachedFieldArrayLayout[i];
            FieldKey key{desc.componentID, desc.fieldIndex};

            // Check if this is a temporal field
            auto temporalIt = TemporalFieldIndices.find(key);
            if (temporalIt != TemporalFieldIndices.end())
            {
                // Get frame 0 pointer from chunk header
                uint8_t* frame0Ptr = static_cast<uint8_t*>(chunk->GetTemporalFieldPointer(temporalIt->second));
                uint8_t* f1Ptr = frame0Ptr + frameOffset_T;
                uint8_t* f2Ptr = frame0Ptr + frameOffset_T1;

                // Interleave read T and write T+1 pointers
                outDualArrayTable[i * 2] = f1Ptr;      // Read from T
                outDualArrayTable[i * 2 + 1] = f2Ptr; // Write to T+1
            }
            else
            {
                // Regular chunk field - same pointer for read and write
                void* ptr = chunkBase + FieldArrayTemplateCache[i].offsetInChunk;
                outDualArrayTable[i * 2] = ptr;
                outDualArrayTable[i * 2 + 1] = ptr;
            }
        }
    }

    // LEGACY: Build field array table with map lookups (slower, for debugging)
    void BuildFieldArrayTableSlow(Chunk* chunk, void** outFieldArrayTable)
    {
        for (size_t i = 0; i < CachedFieldArrayLayout.size(); ++i)
        {
            const auto& desc = CachedFieldArrayLayout[i];

            if (desc.isDecomposed)
            {
                // Decomposed component - get specific field array
                outFieldArrayTable[i] = GetFieldArray(chunk, desc.componentID, desc.fieldIndex);
            }
            else
            {
                // Non-decomposed component - get whole component array
                outFieldArrayTable[i] = GetComponentArrayRaw(chunk, desc.componentID);
            }
        }
    }

    // Get total field array count (for allocating table)
    size_t GetFieldArrayCount() const
    {
        return TotalFieldArrayCount;
    }

    // Validate that cached layout matches current state
    bool ValidateCache() const
    {
        return CachedFieldArrayLayout.size() == TotalFieldArrayCount &&
            FieldArrayTemplateCache.size() == TotalFieldArrayCount;
    }

    // Get component type at specific table index (for debug/validation)
    ComponentTypeID GetComponentTypeAtTableIndex(size_t tableIndex) const
    {
        if (tableIndex >= CachedFieldArrayLayout.size())
            return 0;

        return CachedFieldArrayLayout[tableIndex].componentID;
    }

    // Get field name at specific table index (for debugging)
    const char* GetFieldNameAtTableIndex(size_t tableIndex) const
    {
        if (tableIndex >= FieldArrayTemplateCache.size())
            return "invalid";

        return FieldArrayTemplateCache[tableIndex].debugName;
    }

    // Helper: Align offset to alignment requirement
    static size_t AlignOffset(size_t offset, size_t alignment)
    {
        return (offset + alignment - 1) & ~(alignment - 1);
    }

    // Legacy: Get component array for non-decomposed components
    void* GetComponentArrayRaw(Chunk* chunk, ComponentTypeID typeID)
    {
        auto it = ComponentLayout.find(typeID);
        if (it == ComponentLayout.end()) return nullptr;

        return chunk->GetBuffer(static_cast<uint32_t>(it->second.OffsetInChunk));
    }

private:
    // Allocate a new chunk
    Chunk* AllocateChunk();
};

struct ArchetypeKeyHash
{
    size_t operator()(const Archetype::ArchetypeKey& key) const
    {
        // Start with classID
        size_t hash = key.ID;

        // Mix in signature using FNV-1a
        constexpr size_t FNV_PRIME = 0x100000001b3;
        constexpr size_t FNV_OFFSET = 0xcbf29ce484222325;

        hash = FNV_OFFSET;
        hash ^= key.ID;
        hash *= FNV_PRIME;

        // Process signature in 64-bit chunks
        const uint64_t* data = reinterpret_cast<const uint64_t*>(&key.Sig);
        for (size_t i = 0; i < MAX_COMPONENTS / 64; ++i)
        {
            // 256 bits = 4 × 64-bit chunks
            hash ^= data[i];
            hash *= FNV_PRIME;
        }

        return hash;
    }
};
