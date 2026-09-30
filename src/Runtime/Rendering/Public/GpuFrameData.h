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

constexpr uint32_t MaxGpuFields       = 128;
constexpr uint32_t MaxMeshSlots       = 256;
constexpr uint32_t GpuOutFieldCount   = 16;                                   // Flags + PosXYZ(3) + RotQxyzw(4) + ScaleXYZ(3) + ColorRGBA(4) + MeshID
constexpr uint32_t GpuAnimFieldCount  = 18;                                   // SkeletonID + CAnimBase(10-StateNodeID) + CAnimLayer(8)
constexpr uint32_t GpuTotalFieldCount = GpuOutFieldCount + GpuAnimFieldCount; // 34 slab slots total

// GpuFieldSemantic constants — match values in GpuFrameData.slang.
// Render output fields (sems 1-16): SoA slot in instance buffer = sem - 1.
// Entity cache index (sem 17): always-on, written by scatter from entity loop index.
// Animation slab fields (sems 18-36): live in slab only; scatter skips (sem > GpuOutFieldCount).
//   Slab slot index = FieldDescription array index f (16..34 for anim fields).
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
// Always-on: scatter writes this to instance slot 16 from entity loop index i.
constexpr uint32_t SemEntityCacheIdx = 17;
// Animation slab fields use SemGeneric (0) — scatter skips them via the SemGeneric
// early-out. GPU access goes through SlabIdx_XXX constants in GpuFrameData.slang.

// GpuFrameData::DebugDrawMode values — only written by CPU when TNX_DEBUG_RENDERING.
// The field is always present in the struct (it was _pad0); the shader branch is
// compiled out entirely in shipping builds (no TNX_DEBUG_RENDERING define).
#ifdef TNX_DEBUG_RENDERING
constexpr uint8_t GpuDebugMode_Off      = 0;
constexpr uint8_t GpuDebugMode_SkinPath = 1; // cyan = skeletal, green = static
#endif

// GPU-side mesh slot — mirrors MeshManager::MeshSlot for GPU read.
// 16 bytes, tightly packed for storage buffer access.
struct GpuMeshInfo
{
	uint32_t FirstIndex;
	uint32_t IndexCount;
	int32_t VertexOffset;
	uint32_t _pad; // 4 bytes available
};

static_assert(sizeof(GpuMeshInfo) == 16, "GpuMeshInfo must be 16 bytes");

struct GpuFrameData
{
	float Position[3];
	float OldPosition[3];
	float Rotation[4];
	float OldRotation[4];
	float FoV;
	float OldFoV;
	float AspectRatio;              // offset  64
	float _pad1;                    // offset  68 — 4 bytes available before InstancesAddr
	uint64_t InstancesAddr;         // offset  72 — sorted SoA (draw reads from here)
	uint64_t DrawArgsAddr;          // offset  80
	uint64_t VerticesAddr;          // offset  88
	uint64_t CompactCounterAddr;    // offset  96
	uint64_t ScanAddr;              // offset 104
	float Alpha;                    // offset 112
	uint32_t EntityCount;           // offset 116
	uint32_t FieldCount;            // offset 120
	uint32_t OutFieldStride;        // offset 124
	uint64_t UnsortedInstancesAddr; // offset 128 — scatter writes here
	uint64_t MeshHistogramAddr;     // offset 136
	uint64_t MeshWriteIdxAddr;      // offset 144
	uint64_t MeshTableAddr;         // offset 152
	uint32_t MeshCount;             // offset 160
	uint8_t DebugDrawMode;          // offset 164 — 0=off; non-zero only when TNX_DEBUG_RENDERING
	uint8_t _pad0[3];               // offset 165 — available for future debug flags
	// --- Skinning / animation GPU addresses ---
	uint64_t SkinMatrixAddr;                 // offset 168 — float4x4[MAX_SKELETAL_INSTANCES × MAX_BONES_PER_ENTITY]
	uint64_t SkeletalListAddr;               // offset 176 — uint[MAX_SKELETAL_INSTANCES] compact entity cache indices (GPU-written by scatter)
	uint64_t SkeletalIdxByEntityAddr;        // offset 184 — uint[MAX_CACHED_ENTITIES] cacheIdx → compact skeletal idx (GPU-written by scatter)
	uint64_t GpuBoneDataAddr;                // offset 192 — GpuBoneData[MAX_TOTAL_BONES]
	uint64_t GpuBoneParentAddr;              // offset 200 — uint32[MAX_TOTAL_BONES] parent bone indices
	uint64_t AnimTrackAddr;                  // offset 208 — GpuAnimBoneTrack[MAX_TOTAL_BONE_TRACKS]
	uint64_t AnimKeyframeAddr;               // offset 216 — GpuAnimKeyframe[MAX_TOTAL_KEYFRAMES]
	uint64_t SkinWeightAddr;                 // offset 224 — SkinWeights[MAX_TOTAL_SKIN_WEIGHTS]
	uint64_t SkinSlotTableAddr;              // offset 232 — GpuSkinSlotInfo[MAX_MESH_SLOTS]
	uint64_t SkeletalDispatchArgsAddr;       // offset 240 — VkDispatchIndirectCommand {x=skeletalCount,y=1,z=1}
	uint64_t GpuSkeletonSlotAddr;            // offset 248 — GpuSkeletonSlotInfo[MAX_SKELETON_SLOTS]
	uint64_t GpuAnimSlotAddr;                // offset 256 — GpuAnimSlotInfo[MAX_ANIM_SLOTS]
	uint64_t PrevFieldAddrs[MaxGpuFields];   // offset 264
	uint64_t CurrFieldAddrs[MaxGpuFields];   // offset 1272
	uint32_t FieldSemantics[MaxGpuFields];   // offset 2296
	uint32_t FieldElementSize[MaxGpuFields]; // offset 2808
	float BgInnerColor[4];                   // offset 3320 — radial vignette center color (linear)
	float BgOuterColor[4];                   // offset 3336 — radial vignette edge color (linear)
};

static_assert(sizeof(GpuFrameData) == 3368,
	"GpuFrameData size mismatch — layout must match GpuFrameData.slang exactly");
