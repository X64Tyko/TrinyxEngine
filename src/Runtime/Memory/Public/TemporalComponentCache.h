#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
#include <unordered_map>

#include "Types.h"

struct EngineConfig;
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
    char _padding[ 64 - (sizeof(std::atomic<uint8_t>) + sizeof(uint8_t) + sizeof(uint32_t) * 3 + sizeof(Vector3) * 3 + sizeof(Matrix4) * 2 + sizeof(float)) % 64];
};

class TemporalComponentCache
{
public:
    TemporalComponentCache();
    ~TemporalComponentCache();

    void Initialize(const EngineConfig* Config);

    // O(1) frame header access - external systems manage locking
    TemporalFrameHeader* GetFrameHeader(uint32_t frameNum) const
    {
        return FrameHeaders[frameNum % TemporalFrameCount];
    }
    
    __forceinline uint32_t GetFrameIndex(uint32_t frameNum) const
    {
        return frameNum % TemporalFrameCount;
    }

    // Get next frame with wrapping
    __forceinline uint32_t GetNextFrame(uint32_t currentFrame) const
    {
        return (currentFrame + 1) % TemporalFrameCount;
    }
    
    __forceinline uint32_t GetPrevFrame(uint32_t currentFrame) const
    {
        return (currentFrame + TemporalFrameCount - 1) % TemporalFrameCount;
    }

    // Frame locking for multi-threaded access
    // Returns true if lock was acquired successfully
    bool TryLockFrameForWrite(uint32_t frameNum);

    // Verify frame is not locked for writing (safe to read)
    bool VerifyFrameReadable(uint32_t frameNum) const;

    // Release write lock on frame
    void UnlockFrameWrite(uint32_t frameNum);
    
    bool TryLockFrameForRead(uint32_t frameNum);
    
    void UnlockFrameRead(uint32_t frameNum);

    // Allocate field array for a chunk across all frames, returns absolute pointer to frame 0 data
    // Archetype calls this for each temporal field when allocating a new chunk
    void* AllocateFieldArray(Archetype* owner, class Chunk* chunk, ComponentTypeID compType,
                            size_t fieldIndex, const char* fieldName, size_t entityCount, size_t fieldSize);

    // Get the stride between frames (for calculating frame N from frame 0 pointer)
    __forceinline size_t GetFrameStride() const { return sizeof(TemporalFrameHeader) + FrameDataCapacity; }

    // Get component field data from specific frame
    void* GetFieldData(TemporalFrameHeader* header, ComponentTypeID compType, size_t fieldIndex) const;
    
    uint32_t GetTotalFrameCount() const { return TemporalFrameCount; }

private:
    // Pre-computed field allocation zones (field-major ordering across all archetypes)
    struct FieldAllocationInfo
    {
        ComponentTypeID CompType;
        size_t FieldIndex;
        const char* FieldName;
        size_t OffsetInFrame;      // Start of this field's allocation zone in each frame
        size_t TotalCapacity;      // Max size this field can allocate (sum across all archetypes)
        size_t CurrentUsed;        // How much is currently allocated
    };

    std::vector<FieldAllocationInfo> FieldAllocations;

    // Active allocations with back-refs to chunks for defrag
    struct TemporalAllocation
    {
        Archetype* Owner;
        class Chunk* OwnerChunk;
        size_t FieldAllocationIndex;  // Which FieldAllocationInfo this belongs to
        size_t OffsetInFieldZone;     // Offset within that field's zone
        size_t Size;                  // entityCount * fieldSize (aligned)
    };

    std::vector<TemporalAllocation> ActiveAllocations;

    // Temporal components are allocated in one large slab storing multiple frames of history
    void* SlabPtr = nullptr;
    size_t FrameDataCapacity = 0;      // Max size per frame (all field allocations)
    size_t TemporalFrameCount = 0;     // Number of frames stored in history
    size_t TotalSlabSize = 0;          // Total allocation size

    // O(1) frame access array
    std::vector<TemporalFrameHeader*> FrameHeaders;

    // Helper: Find or create FieldAllocationInfo for a component field
    size_t GetOrCreateFieldAllocationIndex(ComponentTypeID compType, size_t fieldIndex, const char* fieldName);

    // Helper: Align size to FIELD_ARRAY_ALIGNMENT
    static size_t AlignSize(size_t size);
};
