// GpuFrameData.slang — shared GPU frame data struct.
// Included by all compute and graphics shaders via #include "GpuFrameData.slang".
//
// Layout mirrors C++ GpuFrameData in RenderThread.h exactly.
// Accessed via a single uint64_t push constant (buffer device address).

#pragma once

static const uint kMaxGpuFields    = 128;
static const uint kGpuOutFieldCount = 13; // PosXYZ + RotXYZ + ScaleXYZ + ColorRGBA

// GpuFieldSemantic constants (match C++ enum, minus-1 = SoA field slot index)
static const uint kSemGeneric = 0;
static const uint kSemPosX   = 1;  static const uint kSemPosY   = 2;  static const uint kSemPosZ   = 3;
static const uint kSemRotX   = 4;  static const uint kSemRotY   = 5;  static const uint kSemRotZ   = 6;
static const uint kSemScaleX = 7;  static const uint kSemScaleY = 8;  static const uint kSemScaleZ = 9;
static const uint kSemColorR = 10; static const uint kSemColorG = 11; static const uint kSemColorB = 12; static const uint kSemColorA = 13;

// Persistently-mapped per-frame GPU struct.
// A single 8-byte push constant holds its device address.
//
//  C++ offset   Field
//       0        ViewProj[16]           — column-major view-projection matrix
//      64        InstancesAddr          — SoA output: kGpuOutFieldCount × OutFieldStride floats
//      72        DrawArgsAddr           — VkDrawIndexedIndirectCommand
//      80        FlagsAddr              — uint per entity (bit 31 = active)
//      88        VerticesAddr           — mesh vertex buffer (BDA read in vertex shader)
//      96        CompactCounterAddr     — single uint: global atomicAdd target
//     104        ScanAddr               — uint per entity: exclusive-scan output index
//     112        Alpha                  — render interpolation [0.0, 1.0]
//     116        EntityCount
//     120        FieldCount             — valid entries in Prev/CurrFieldAddrs
//     124        OutFieldStride         — element stride between SoA output fields
//     128        PrevFieldAddrs[128]    — frame T-1 SoA device addresses
//    1152        CurrFieldAddrs[128]    — frame T   SoA device addresses
//    2176        FieldSemantics[128]    — GpuFieldSemantic per field
//    2688        FieldElementSize[128]  — bytes per entity per field
//    3200        (total)
struct GpuFrameData
{
    float    ViewProj[16];
    uint64_t InstancesAddr;
    uint64_t DrawArgsAddr;
    uint64_t FlagsAddr;
    uint64_t VerticesAddr;
    uint64_t CompactCounterAddr;
    uint64_t ScanAddr;
    float    Alpha;
    uint     EntityCount;
    uint     FieldCount;
    uint     OutFieldStride;
    uint64_t PrevFieldAddrs[kMaxGpuFields];
    uint64_t CurrFieldAddrs[kMaxGpuFields];
    uint     FieldSemantics[kMaxGpuFields];
    uint     FieldElementSize[kMaxGpuFields];
};
