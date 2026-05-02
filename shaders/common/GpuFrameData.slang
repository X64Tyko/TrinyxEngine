// GpuFrameData.slang — shared GPU frame data struct.
// Included by all compute and graphics shaders via #include "common/GpuFrameData.slang".
//
// Layout mirrors C++ GpuFrameData in GpuFrameData.h exactly.
// Accessed via a single uint64_t push constant (buffer device address).

#pragma once

static const uint MaxGpuFields     = 128;
static const uint MaxMeshSlots     = 256;
static const uint GpuOutFieldCount = 16; // Flags + PosXYZ + RotQxyzw + ScaleXYZ + ColorRGBA + MeshID

// GpuFieldSemantic constants (match C++ GpuFrameData.h, slot index = sem - 1)
static const uint SemGeneric = 0;
static const uint SemFlags   = 1;
static const uint SemPosX    = 2;  static const uint SemPosY    = 3;  static const uint SemPosZ    = 4;
static const uint SemRotQx   = 5;  static const uint SemRotQy   = 6;  static const uint SemRotQz   = 7;  static const uint SemRotQw = 8;
static const uint SemScaleX  = 9;  static const uint SemScaleY  = 10; static const uint SemScaleZ  = 11;
static const uint SemColorR  = 12; static const uint SemColorG  = 13; static const uint SemColorB  = 14; static const uint SemColorA = 15;
static const uint SemMeshID  = 16;

// Under TNX_GPU_PICKING, scatter writes the entity's cache index into this
// output SoA slot so the vertex/fragment pipeline can forward it to a uint
// pick attachment. The CPU resolves cache index → EntityRecord on readback.
#ifdef TNX_GPU_PICKING
static const uint GpuPickFieldCount = 17; // GpuOutFieldCount + 1 (EntityCacheIdx)
static const uint SemEntityCacheIdx = 17;
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
    float Position[3];
    float OldPosition[3];
    float Rotation[4];
    float OldRotation[4];
    float FoV;
    float OldFoV;
    float    AspectRatio;              // viewport width / height
    float    _pad1;
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
    uint64_t PrevFieldAddrs[MaxGpuFields];
    uint64_t CurrFieldAddrs[MaxGpuFields];
    uint     FieldSemantics[MaxGpuFields];
    uint     FieldElementSize[MaxGpuFields];
};