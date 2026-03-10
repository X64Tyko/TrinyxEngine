// GpuFrameData.slang — shared GPU frame data struct.
// Included by all compute and graphics shaders via #include "GpuFrameData.slang".
//
// Layout mirrors C++ GpuFrameData in RenderThread.h exactly.
// Accessed via a single uint64_t push constant (buffer device address).

#pragma once

static const uint kMaxGpuFields    = 128;
static const uint kGpuOutFieldCount = 15; // Flags + PosXYZ + RotQxyzw + ScaleXYZ + ColorRGBA

// GpuFieldSemantic constants (match C++ GpuFrameData.h, slot index = sem - 1)
static const uint kSemGeneric = 0;
static const uint kSemFlags   = 1;
static const uint kSemPosX    = 2;  static const uint kSemPosY    = 3;  static const uint kSemPosZ    = 4;
static const uint kSemRotQx   = 5;  static const uint kSemRotQy   = 6;  static const uint kSemRotQz   = 7;  static const uint kSemRotQw = 8;
static const uint kSemScaleX  = 9;  static const uint kSemScaleY  = 10; static const uint kSemScaleZ  = 11;
static const uint kSemColorR  = 12; static const uint kSemColorG  = 13; static const uint kSemColorB  = 14; static const uint kSemColorA = 15;

// Persistently-mapped per-frame GPU struct.
// A single 8-byte push constant holds its device address.
//
//  C++ offset   Field
//       0        ViewProj[16]           — column-major view-projection matrix
//      64        InstancesAddr          — SoA output: kGpuOutFieldCount × OutFieldStride floats
//      72        DrawArgsAddr           — VkDrawIndexedIndirectCommand
//      80        VerticesAddr           — mesh vertex buffer (BDA read in vertex shader)
//      88        CompactCounterAddr     — single uint: global atomicAdd target
//      96        ScanAddr               — uint per entity: exclusive-scan output index
//     104        Alpha                  — render interpolation [0.0, 1.0]
//     108        EntityCount
//     112        FieldCount             — valid entries in Prev/CurrFieldAddrs
//     116        OutFieldStride         — element stride between SoA output fields
//     120        PrevFieldAddrs[128]    — frame T-1 SoA device addresses
//    1144        CurrFieldAddrs[128]    — frame T   SoA device addresses
//    2168        FieldSemantics[128]    — GpuFieldSemantic per field
//    2680        FieldElementSize[128]  — bytes per entity per field
//    3192        (total)
//
// Flags are read from CurrFieldAddrs — the field with kSemFlags semantic
// is always at field index 0 (convention enforced by FillGpuFrameData).
struct GpuFrameData
{
    float    ViewProj[16];
    uint64_t InstancesAddr;
    uint64_t DrawArgsAddr;
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
