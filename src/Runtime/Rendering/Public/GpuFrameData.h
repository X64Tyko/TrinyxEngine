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
// Flags are read from CurrFieldAddrs — the field with SemFlags semantic
// is always at field index 0 (convention enforced by FillGpuFrameData).
// ---------------------------------------------------------------------------

constexpr uint32_t MaxGpuFields     = 128;
constexpr uint32_t MaxMeshSlots     = 256;
constexpr uint32_t GpuOutFieldCount = 16; // Flags + PosXYZ(3) + RotQxyzw(4) + ScaleXYZ(3) + ColorRGBA(4) + MeshID

// GpuFieldSemantic constants — match values in GpuFrameData.slang.
// SoA slot index for field k = sem - 1.
constexpr uint32_t SemGeneric = 0;
constexpr uint32_t SemFlags   = 1;
constexpr uint32_t SemPosX    = 2;
constexpr uint32_t SemPosY    = 3;
constexpr uint32_t SemPosZ    = 4;
constexpr uint32_t SemRotQx   = 5;
constexpr uint32_t SemRotQy   = 6;
constexpr uint32_t SemRotQz   = 7;
constexpr uint32_t SemRotQw   = 8;
constexpr uint32_t SemScaleX  = 9;
constexpr uint32_t SemScaleY  = 10;
constexpr uint32_t SemScaleZ  = 11;
constexpr uint32_t SemColorR  = 12;
constexpr uint32_t SemColorG  = 13;
constexpr uint32_t SemColorB  = 14;
constexpr uint32_t SemColorA  = 15;
constexpr uint32_t SemMeshID  = 16;

// GPU-side mesh slot — mirrors MeshManager::MeshSlot for GPU read.
// 16 bytes, tightly packed for storage buffer access.
struct GpuMeshInfo
{
	uint32_t FirstIndex;
	uint32_t IndexCount;
	int32_t VertexOffset;
	uint32_t _pad;
};

static_assert(sizeof(GpuMeshInfo) == 16, "GpuMeshInfo must be 16 bytes");

struct GpuFrameData
{
	float ViewProj[16];                       // offset   0
	uint64_t InstancesAddr;                   // offset  64 — sorted SoA (draw reads from here)
	uint64_t DrawArgsAddr;                    // offset  72
	uint64_t VerticesAddr;                    // offset  80
	uint64_t CompactCounterAddr;              // offset  88
	uint64_t ScanAddr;                        // offset  96
	float Alpha;                              // offset 104
	uint32_t EntityCount;                     // offset 108
	uint32_t FieldCount;                      // offset 112
	uint32_t OutFieldStride;                  // offset 116
	uint64_t UnsortedInstancesAddr;           // offset 120 — scatter writes here
	uint64_t MeshHistogramAddr;               // offset 128
	uint64_t MeshWriteIdxAddr;                // offset 136
	uint64_t MeshTableAddr;                   // offset 144
	uint32_t MeshCount;                       // offset 152
	uint32_t _pad0;                           // offset 156
	uint64_t PrevFieldAddrs[MaxGpuFields];   // offset 160
	uint64_t CurrFieldAddrs[MaxGpuFields];   // offset 1184
	uint32_t FieldSemantics[MaxGpuFields];   // offset 2208
	uint32_t FieldElementSize[MaxGpuFields]; // offset 2720
};                                            // total  3232

static_assert(sizeof(GpuFrameData) == 3232,
			  "GpuFrameData size mismatch — layout must match GpuFrameData.slang exactly");