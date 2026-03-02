#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// GpuFrameData — C++ mirror of GpuFrameData.slang
//
// The CPU writes one of these into each frame slot's PersistentMapped GpuData
// buffer before recording the command buffer.  A single uint64_t push constant
// carries its buffer device address to the vertex shader.
//
// Layout is byte-for-byte identical to GpuFrameData.slang; the static_assert
// below enforces the match at compile time.
//
// Offset map (matches shader comments):
//     0   ViewProj[16]           col-major view-projection matrix
//    64   InstancesAddr          BDA → SoA floats (kGpuOutFieldCount × OutFieldStride)
//    72   DrawArgsAddr           BDA → VkDrawIndexedIndirectCommand
//    80   FlagsAddr              BDA → uint per entity (bit 31 = active)
//    88   VerticesAddr           BDA → Vertex[24] mesh geometry
//    96   CompactCounterAddr     BDA → single uint (atomicAdd target)
//   104   ScanAddr               BDA → uint per entity (exclusive-scan index)
//   112   Alpha                  render interpolation [0.0, 1.0]
//   116   EntityCount
//   120   FieldCount             valid entries in Prev/CurrFieldAddrs
//   124   OutFieldStride         element stride between SoA output fields
//   128   PrevFieldAddrs[128]    frame T-1 SoA device addresses
//  1152   CurrFieldAddrs[128]    frame T   SoA device addresses
//  2176   FieldSemantics[128]    GpuFieldSemantic per field
//  2688   FieldElementSize[128]  bytes per entity per field
//  3200   (total)
// ---------------------------------------------------------------------------

constexpr uint32_t kMaxGpuFields    = 128;
constexpr uint32_t kGpuOutFieldCount = 13; // PosXYZ(3) + RotXYZ(3) + ScaleXYZ(3) + ColorRGBA(4)

// GpuFieldSemantic constants — match kSem* values in GpuFrameData.slang.
// SoA slot index for field k = kSem - 1.
constexpr uint32_t kSemGeneric = 0;
constexpr uint32_t kSemPosX    = 1;  constexpr uint32_t kSemPosY   = 2;  constexpr uint32_t kSemPosZ   = 3;
constexpr uint32_t kSemRotX    = 4;  constexpr uint32_t kSemRotY   = 5;  constexpr uint32_t kSemRotZ   = 6;
constexpr uint32_t kSemScaleX  = 7;  constexpr uint32_t kSemScaleY = 8;  constexpr uint32_t kSemScaleZ = 9;
constexpr uint32_t kSemColorR  = 10; constexpr uint32_t kSemColorG = 11; constexpr uint32_t kSemColorB = 12; constexpr uint32_t kSemColorA = 13;

struct GpuFrameData
{
	float    ViewProj[16];                    // offset   0
	uint64_t InstancesAddr;                   // offset  64
	uint64_t DrawArgsAddr;                    // offset  72
	uint64_t FlagsAddr;                       // offset  80
	uint64_t VerticesAddr;                    // offset  88
	uint64_t CompactCounterAddr;              // offset  96
	uint64_t ScanAddr;                        // offset 104
	float    Alpha;                           // offset 112
	uint32_t EntityCount;                     // offset 116
	uint32_t FieldCount;                      // offset 120
	uint32_t OutFieldStride;                  // offset 124
	uint64_t PrevFieldAddrs[kMaxGpuFields];   // offset 128
	uint64_t CurrFieldAddrs[kMaxGpuFields];   // offset 1152
	uint32_t FieldSemantics[kMaxGpuFields];   // offset 2176
	uint32_t FieldElementSize[kMaxGpuFields]; // offset 2688
};                                            // total   3200

static_assert(sizeof(GpuFrameData) == 3200,
	"GpuFrameData size mismatch — layout must match GpuFrameData.slang exactly");
