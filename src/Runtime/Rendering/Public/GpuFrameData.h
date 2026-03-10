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
// Offset map (matches GpuFrameData.slang):
//     0   ViewProj[16]           col-major view-projection matrix
//    64   InstancesAddr          BDA → SoA floats (kGpuOutFieldCount × OutFieldStride)
//    72   DrawArgsAddr           BDA → VkDrawIndexedIndirectCommand
//    80   VerticesAddr           BDA → Vertex[24] mesh geometry
//    88   CompactCounterAddr     BDA → single uint (atomicAdd target)
//    96   ScanAddr               BDA → uint per entity (exclusive-scan index)
//   104   Alpha                  render interpolation [0.0, 1.0]
//   108   EntityCount
//   112   FieldCount             valid entries in Prev/CurrFieldAddrs
//   116   OutFieldStride         element stride between SoA output fields
//   120   PrevFieldAddrs[128]    frame T-1 SoA device addresses
//  1144   CurrFieldAddrs[128]    frame T   SoA device addresses
//  2168   FieldSemantics[128]    GpuFieldSemantic per field
//  2680   FieldElementSize[128]  bytes per entity per field
//  3192   (total)
//
// Flags are read from CurrFieldAddrs — the field with kSemFlags semantic
// is always at field index 0 (convention enforced by FillGpuFrameData).
// ---------------------------------------------------------------------------

constexpr uint32_t kMaxGpuFields     = 128;
constexpr uint32_t kGpuOutFieldCount = 15; // Flags + PosXYZ(3) + RotQxyzw(4) + ScaleXYZ(3) + ColorRGBA(4)

// GpuFieldSemantic constants — match kSem* values in GpuFrameData.slang.
// SoA slot index for field k = kSem - 1.
constexpr uint32_t kSemGeneric = 0;
constexpr uint32_t kSemFlags   = 1;
constexpr uint32_t kSemPosX    = 2;
constexpr uint32_t kSemPosY    = 3;
constexpr uint32_t kSemPosZ    = 4;
constexpr uint32_t kSemRotQx   = 5;
constexpr uint32_t kSemRotQy   = 6;
constexpr uint32_t kSemRotQz   = 7;
constexpr uint32_t kSemRotQw   = 8;
constexpr uint32_t kSemScaleX  = 9;
constexpr uint32_t kSemScaleY  = 10;
constexpr uint32_t kSemScaleZ  = 11;
constexpr uint32_t kSemColorR  = 12;
constexpr uint32_t kSemColorG  = 13;
constexpr uint32_t kSemColorB  = 14;
constexpr uint32_t kSemColorA  = 15;

struct GpuFrameData
{
	float ViewProj[16];                       // offset   0
	uint64_t InstancesAddr;                   // offset  64
	uint64_t DrawArgsAddr;                    // offset  72
	uint64_t VerticesAddr;                    // offset  88
	uint64_t CompactCounterAddr;              // offset  96
	uint64_t ScanAddr;                        // offset 104
	float Alpha;                              // offset 112
	uint32_t EntityCount;                     // offset 116
	uint32_t FieldCount;                      // offset 120
	uint32_t OutFieldStride;                  // offset 124
	uint64_t PrevFieldAddrs[kMaxGpuFields];   // offset 128
	uint64_t CurrFieldAddrs[kMaxGpuFields];   // offset 1152
	uint32_t FieldSemantics[kMaxGpuFields];   // offset 2176
	uint32_t FieldElementSize[kMaxGpuFields]; // offset 2688
};                                            // total  3192

static_assert(sizeof(GpuFrameData) == 3192,
			  "GpuFrameData size mismatch — layout must match GpuFrameData.slang exactly");
