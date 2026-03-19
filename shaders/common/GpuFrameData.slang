// GpuFrameData.slang — shared GPU frame data struct.
// Included by all compute and graphics shaders via #include "common/GpuFrameData.slang".
//
// Layout mirrors C++ GpuFrameData in GpuFrameData.h exactly.
// Accessed via a single uint64_t push constant (buffer device address).

#pragma once

static const uint kMaxGpuFields     = 128;
static const uint kMaxMeshSlots     = 256;
static const uint kGpuOutFieldCount = 16; // Flags + PosXYZ + RotQxyzw + ScaleXYZ + ColorRGBA + MeshID

// GpuFieldSemantic constants (match C++ GpuFrameData.h, slot index = sem - 1)
static const uint kSemGeneric = 0;
static const uint kSemFlags   = 1;
static const uint kSemPosX    = 2;  static const uint kSemPosY    = 3;  static const uint kSemPosZ    = 4;
static const uint kSemRotQx   = 5;  static const uint kSemRotQy   = 6;  static const uint kSemRotQz   = 7;  static const uint kSemRotQw = 8;
static const uint kSemScaleX  = 9;  static const uint kSemScaleY  = 10; static const uint kSemScaleZ  = 11;
static const uint kSemColorR  = 12; static const uint kSemColorG  = 13; static const uint kSemColorB  = 14; static const uint kSemColorA = 15;
static const uint kSemMeshID  = 16;

// Under TNX_GPU_PICKING, scatter writes the entity's cache index into this
// output SoA slot so the vertex/fragment pipeline can forward it to a uint
// pick attachment. The CPU resolves cache index → EntityRecord on readback.
#ifdef TNX_GPU_PICKING
static const uint kGpuPickFieldCount = 17; // kGpuOutFieldCount + 1 (EntityCacheIdx)
static const uint kSemEntityCacheIdx = 17;
#endif

// GPU-side mesh slot — mirrors MeshManager::MeshSlot for GPU read.
struct GpuMeshInfo
{
    uint FirstIndex;
    uint IndexCount;
    int  VertexOffset;
    uint _pad;
};

// Persistently-mapped per-frame GPU struct.
// A single 8-byte push constant holds its device address.
struct GpuFrameData
{
    float    ViewProj[16];
    uint64_t InstancesAddr;            // sorted SoA — vertex shader reads from here
    uint64_t DrawArgsAddr;             // VkDrawIndexedIndirectCommand[MeshCount]
    uint64_t VerticesAddr;             // mesh vertex buffer (BDA)
    uint64_t CompactCounterAddr;       // single uint: global atomicAdd target
    uint64_t ScanAddr;                 // uint per entity: exclusive-scan output index
    float    Alpha;                    // render interpolation [0.0, 1.0]
    uint     EntityCount;
    uint     FieldCount;               // valid entries in Prev/CurrFieldAddrs
    uint     OutFieldStride;           // element stride between SoA output fields
    uint64_t UnsortedInstancesAddr;    // scatter writes here (pre-sort)
    uint64_t MeshHistogramAddr;        // uint[256] counting sort histogram
    uint64_t MeshWriteIdxAddr;         // uint[256] atomic write indices for sort
    uint64_t MeshTableAddr;            // GpuMeshInfo[MeshCount]
    uint     MeshCount;                // number of registered mesh types
    uint     _pad0;
    uint64_t PrevFieldAddrs[kMaxGpuFields];
    uint64_t CurrFieldAddrs[kMaxGpuFields];
    uint     FieldSemantics[kMaxGpuFields];
    uint     FieldElementSize[kMaxGpuFields];
};