# StrigidEngine Architecture

> **Navigation:** [← Back to README](../README.md) | [Performance Targets →](PERFORMANCE_TARGETS.md)

---

# Threading Model: The Strigid Trinity

## Overview

StrigidEngine uses a **three-thread architecture** with job-based parallelism:

1. **Sentinel (Main Thread):** 1000Hz input polling, window + Vulkan object lifetime, frame pacing
2. **Brain (Logic Thread):** 512Hz fixed timestep coordinator + job distribution
3. **Encoder (Render Thread):** Variable-rate render coordinator + job distribution

**Key Design:** Brain and Encoder are **coordinators**, not workers. They initialize frames, distribute work to
the job system, and act as workers themselves while jobs are pending.

## Job System Architecture

### Core Count Distribution (8-core example)

- **Sentinel Thread:** 1 dedicated core (input + Vulkan lifetime)
- **Encoder Thread:** 1 dedicated core (render coordination + render worker)
- **Brain Thread:** 1 dedicated core (logic coordination + logic worker)
- **Workers:** 5 cores (process tasks from both queues via work-stealing)

### Queue Affinity Rules

- **Brain Thread:** Only pulls from `LogicQueue` (when acting as worker)
- **Encoder Thread:** Only pulls from `RenderQueue` (when acting as worker)
- **Generic Workers:** Pull from both queues (work-stealing)

This prevents Brain from accidentally processing render jobs and vice versa, while generic workers balance load
across both queues.

### Job Types

**Logic Jobs:** PrePhysics updates (per-chunk), physics simulation, PostPhysics updates

**Render Jobs:** GPU upload (dirty entity delta), compute dispatch (predicate/prefix_sum/scatter),
frustum culling (planned), sort key generation (planned), command encoding

---

# Memory Model: Tiered Storage

## Overview: Four Storage Tiers

Entity component data lives in one of four tiers based on access pattern and rollback requirements:

| Tier | Structure | Frames | Rollback | Use Case |
|------|-----------|--------|----------|----------|
| Cold | Archetype chunks (AoS) | 0 | No | Rarely-updated, non-iterable data |
| Static | Separate read-only array | 0 | No | Geometry/mesh data, never changes |
| Volatile | SoA ring buffer | 5 | No | Cosmetic entities, ambient AI, particles |
| Temporal | SoA ring buffer | max(8, X) | Yes | Networked, simulation-authoritative entities |

**Cold** components (no `STRIGID_TEMPORAL_FIELDS`) live only in Archetype chunks (AoS layout).
They are rarely accessed by inner loops — inventory, quest state, AI decision trees.

**Static** entities get a dedicated read-only SoA array. No rollback overhead. Physics and render include
static data as a third dense pass after their partition ranges.

**Volatile** entities use a 5-frame ring buffer per field. 5 frames gives Logic/4 headroom between
thread tick rates (4 frames = Logic/2 — tighter). No rollback; these entities are not networked.

**Temporal** entities use an N-frame ring buffer where N = `max(8, rollback_depth_at_FixedUpdateHz)`.
Full history for rollback netcode, lag compensation, and replay.

---

## Volatile vs Temporal Auto-Classification

A single marker component selects the tier:

```cpp
struct SimulationBody { /* empty marker, no fields */ };
```

- Entity **has** `SimulationBody` → **Temporal** tier (full rollback ring buffer)
- Entity **does not have** `SimulationBody` → **Volatile** tier (5-frame ring buffer, no rollback)

The same component types (Transform, Velocity, etc.) can live in either tier depending on the entity.
A wandering ambient bird and a networked player both have a Transform, but only the player is Temporal.

---

## Partition Design

Within each tier's slab, entities are organized into fixed-size partitions by system access group:

```
DUAL    [0 .. MAX_DUAL)              ← physics + render access (players, AI, physics props)
PHYS    [0 .. MAX_PHYS)              ← physics-only (trigger volumes, invisible movers)
RENDER  [0 .. MAX_RENDER)            ← render-only (particles, decals, ambient props)
LOGIC   [MAX_DUAL+MAX_PHYS+MAX_RENDER .. MAX_DYNAMIC)   ← remainder
```

Config constraint (validated at startup):
```
assert(MaxDualEntities + MaxPhysEntities + MaxRenderEntities <= MaxDynamicEntities)
```

**System iteration patterns (three dense passes each, no pointer chasing):**
- Physics iterates: `DUAL[0..N_dual)` → `PHYS[0..N_phys)` → `STATIC[0..N_static)`
- Render iterates:  `DUAL[0..N_dual)` → `RENDER[0..N_render)` → `STATIC[0..N_static)`

The partition boundaries are fixed at engine startup from `EngineConfig`. No entity can push another
into a different partition at runtime — the regions are pre-allocated.

---

## Entity Group Auto-Derivation

The partition group (Dual/Phys/Render/Logic) is computed automatically at `STRIGID_REGISTER_ENTITY`
time (static initialization), derived from the SystemGroup tags on each component:

```cpp
// Component system group annotations — no manual entity group annotation needed
STRIGID_TEMPORAL_FIELDS(RigidBody, SystemGroup::Phys,   VelX, VelY, VelZ)
STRIGID_TEMPORAL_FIELDS(Material,  SystemGroup::Render, ColorR, ColorG, ColorB)
STRIGID_TEMPORAL_FIELDS(Transform, SystemGroup::None,   PosX, PosY, PosZ)  // partition-agnostic
STRIGID_TEMPORAL_FIELDS(Stats,     SystemGroup::Logic,  Health, Ammo)
```

Derivation rule (applied once at static-init when `STRIGID_REGISTER_ENTITY(T)` fires):

| Components present | → Entity Group |
|--------------------|----------------|
| Phys AND Render | Dual |
| Phys only | Phys |
| Render only | Render |
| Temporal but neither | Logic |
| No temporal components at all | Non-temporal (flags live in chunk) |

`STRIGID_REGISTER_ENTITY(T)` takes **no manual group annotation** — fully automatic. Giving users a
manual override is giving them a gun to shoot gaps into their partition regions with.

---

## Universal Strip (Flags)

Active/Dirty flags live **outside** the per-partition field zones in a dedicated universal strip:

```
┌─────────────────────────────────────────────────────────────────────┐
│  Universal Strip (one contiguous int32 array = one SIMD pass)       │
│  Flags[0..MAX_DYNAMIC_ENTITIES)   ← TemporalFlagBits::Active | Dirty│
├─────────────────────────────────────────────────────────────────────┤
│  DUAL Partition [0..MAX_DUAL)                                        │
│    PosX[MAX_DUAL], PosY[MAX_DUAL], PosZ[MAX_DUAL], ...              │
│    VelX[MAX_DUAL], VelY[MAX_DUAL], VelZ[MAX_DUAL], ...              │
├─────────────────────────────────────────────────────────────────────┤
│  PHYS Partition [0..MAX_PHYS)                                        │
│    VelX[MAX_PHYS], ...                                              │
├─────────────────────────────────────────────────────────────────────┤
│  RENDER Partition [0..MAX_RENDER)                                    │
│    ColorR[MAX_RENDER], ...                                           │
├─────────────────────────────────────────────────────────────────────┤
│  LOGIC Partition [remainder]                                         │
└─────────────────────────────────────────────────────────────────────┘
```

Macro: `STRIGID_UNIVERSAL_COMPONENT(TemporalFlags, Flags)`

- **Temporal/Volatile entities:** flags in universal strip of their slab (one contiguous array = one SIMD pass
  for ALL entities regardless of partition)
- **Non-temporal entities:** flags live in archetype chunk alongside cold data

Current implementation uses `TemporalFlagBits::Active = 1<<31` and `TemporalFlagBits::Dirty = 1<<30`.
The dirty bit doubles as the GPU upload trigger and (during rollback) as a selective resimulation marker —
the dirty set from any correction expands naturally through update logic to the exact blast radius.

---

# Fixed-Point Coordinate System

## Motivation

Float32 within a 1km space partition cell has ≈8mm precision and is non-deterministic across platforms
(different FPU rounding modes, compiler optimizations, FMA behavior). For rollback netcode — where two
clients running the same inputs must produce bit-identical simulation state — this is unacceptable.

Fixed-point int32 is perfectly deterministic on every platform, every compiler, every optimization level.
Integer arithmetic produces identical bits on x86, ARM, any future target. This is the gold standard for
deterministic simulation and the reason fighting game engines (which require frame-perfect rollback) have
used it for decades.

## Unit Definition

```
1 unit = 0.1mm (100 micrometers)
1 meter   =     10,000 units
1 km      = 10,000,000 units
int32 max = 2,147,483,647 units = ±214 km

Cell size (1km): max value = 5,000,000 units  →  430× headroom before overflow
```

This unit definition is baked into all component field definitions. Changing it later requires migrating
all stored data — choose once, document clearly.

## Fixed32 Value Type

A `Fixed32` wrapper around `int32_t` with overloaded arithmetic handles the multiply-shift pattern
transparently. Entity authors write natural syntax; the engine handles int64 intermediates:

```cpp
// Multiply: (a × b) needs 64-bit intermediate to avoid overflow
inline Fixed32 operator*(Fixed32 a, Fixed32 b) {
    return Fixed32(static_cast<int32_t>(
        (static_cast<int64_t>(a.raw) * b.raw) >> FIXED_SCALE_SHIFT
    ));
}

// Entity authors write exactly the same syntax as float:
transform.PosX += body.VelX * dt;   // Fixed32 * Fixed32 → Fixed32, no overflow
```

`FieldProxy<Fixed32, WIDTH>` — all position, velocity, and force fields use this type.

## Quaternion Representation

Quaternion components live in [-1, 1]. Store as int32 in 0.31 fixed-point format
(1 sign bit, 31 fraction bits). Quaternion multiplication is pure integer math with int64
intermediates — no trig in the inner loop. Trig only appears when creating a quaternion
from Euler angles (rare: at spawn or explicit rotation assignment).

## Coordinate Layers

```
Cell-relative fixed-point (int32):
  Entity positions, velocities, forces — all logic and physics

Cell world origin (float64 or int64):
  Absolute world position of each cell's origin
  Stored in Registry/World, never in entity SoA

Camera world position (float64 or int64):
  Maintained by logic thread, published each frame
```

## Render Thread Conversion (Upload Time)

The only lossy step — done once per dirty entity per frame by the render thread:

```cpp
// Camera-relative float32 for GPU (precision loss is acceptable: ≈0.05mm at 1km)
float32 gpu_x = float32(
    double(cell_world_origin.x - camera_world_pos.x)   // float64 subtraction (CPU, free)
    + double(entity.PosX.raw) * 0.0001                 // fixed-point units → meters
);
```

GPU receives small float32 (camera-relative, ≤±1km). Full float32 precision applies. Full throughput.
No doubles on the GPU.

## Jolt Physics Bridge (Temporary)

Jolt uses float32 internally. At the physics boundary:

```
Our fixed-point int32 → float32 cell-relative → Jolt body position
Jolt result float32   → int32 fixed-point     → our slab
```

At cell-relative scale (≤±500m from cell center), float32 precision is ≈0.05mm — finer than our
0.1mm unit definition. The bridge is lossless. When Jolt is replaced with a custom solver, the
bridge goes away and the fixed-point data feeds the solver directly.

---

# Constraint System

## Design: Constraint Pool (Separate AoS, Not in Temporal Slab)

Constraints are structural metadata — which body, which anchor, which type. They don't evolve
frame-to-frame and don't need rollback history. The *resolved result* of constraints (world transforms
of constrained entities) lives in the temporal SoA as normal entity data. The constraint relationships
themselves live in a separate flat AoS pool.

AoS is correct here (vs SoA for entity fields) because the solver always reads all fields of one
constraint together — it never needs "all BodyA values" in isolation. Cache access pattern matches AoS.

Developers who need temporal/dynamic constraints (constraints that change per-frame, need history,
or need rollback) can author their own constraint components using the standard temporal SoA system.
The engine doesn't pay for that complexity by default.

```
ConstraintPool (flat AoS, owned by Registry, separate from temporal slab):

  ConstraintData[MAX_CONSTRAINTS]:
    uint32_t       BodyA        — entity partition index (UINT32_MAX = static world anchor)
    uint32_t       BodyB        — entity partition index (UINT32_MAX = static world anchor)
    ConstraintType Type         — Rigid, Hinge, BallSocket, Prismatic, Distance, Spring, ...
    uint16_t       BoneA        — bone on BodyA (UINT16_MAX = entity root pivot)
    uint16_t       BoneB        — bone on BodyB (UINT16_MAX = entity root pivot)
    Fixed32        AnchorA[3]   — local-space anchor on BodyA
    Fixed32        AnchorB[3]   — local-space anchor on BodyB
    Fixed32        LimitMin[3]  — lower DOF limits (translation or angle)
    Fixed32        LimitMax[3]  — upper DOF limits
    Fixed32        Stiffness    — spring/soft constraints
    Fixed32        Damping      — spring/soft constraints

  uint32_t Count
  uint32_t ByBodyA[MAX_DYNAMIC_ENTITIES]  — O(1) lookup: "what constraint has entity X as BodyA?"
                                            UINT32_MAX = entity is unconstrained (physics root)
```

Why a separate AoS pool rather than a component on the child:
- Rigid attachment (weapon to hand) is just `Type=Rigid, all 6 DOF locked` — a degenerate case of the
  general form, not a special system
- World-anchored constraints (spring to fixed point in space) are natural: `BodyA = world`
- Multiple constraints between two bodies are possible (door: hinge + spring, no component slot conflict)
- Every Jolt joint type maps directly to a `ConstraintType` enum value
- Replacing the physics solver means replacing the solver loop, not the data layout

## Constraint Types

```cpp
enum class ConstraintType : uint8_t {
    Rigid,          // all 6 DOF locked — rigid attachment, transform inheritance
    RigidWithScale, // rigid + scale inherited (careful: breaks physics proxy)
    PositionOnly,   // 3 translation DOF locked, rotation free
    Hinge,          // 1 rotational DOF free (door, wheel on axle)
    BallSocket,     // 3 rotational DOF free, translation locked (shoulder joint)
    Prismatic,      // 1 translational DOF free, rotation locked (piston, slider)
    Distance,       // fixed distance between anchors, all rotation free (rope segment)
    Spring,         // distance constraint with stiffness/damping (soft attach, cloth edge)
};
```

## Naming: ConstraintEntity vs Jolt Constraints

Jolt's internal "constraints" (its joint system) are a different concept. To avoid confusion:
- **ConstraintEntity** (our system) = relationship between two ECS entities, drives transform and physics
- **Jolt joints** = Jolt's internal constraint solver, used only while Jolt is the physics backend

When Jolt is replaced, Jolt joints disappear. ConstraintEntities remain — the custom solver reads them.

## Render Thread Rigid Attachment Pass

Before uploading dirty entity deltas, the render thread resolves `Type=Rigid` constraints:

```
For each ConstraintEntity where Type == Rigid:
    WorldTransform[BodyA] = WorldTransform[BodyB]  (resolved via BoneB if BoneB != INVALID)
                            ⊗ LocalOffset(AnchorB → AnchorA)
    Mark BodyA dirty if BodyB was dirty this frame
```

Ordering: BodyB (parent) must be processed before BodyA (child). Two-pass iteration covers depth ≤ 2
(the vast majority of real attachment hierarchies). Cap at 4 passes, assert if exceeded.

## Physics: Only Roots Are Simulated

Entities with a `Type=Rigid` ConstraintEntity pointing to them as BodyA are **not** given independent
physics bodies. They inherit their transform from BodyB. The physics solver only creates bodies for
root entities (no ConstraintEntity where they are BodyA with Type=Rigid).

Entities that need independent physics AND follow a parent (ragdoll limbs, physically-simulated rope
segments) use non-Rigid constraint types — the solver resolves them via impulses/forces, not transform
copy. This is the distinction between transform attachment (render thread, free) and physics constraints
(solver, paid in simulation cost).

---

## Current Implementation vs Design

The full tiered partition design is **designed but not yet implemented**. The current code uses
`TemporalComponentCache` as a dual-buffer SoA approximation (Read/Write arrays per field, one frame of
history). It correctly tracks the Active and Dirty bits per entity and supports the delta-upload path.

**Known issue in current TemporalComponentCache:** Cross-archetype co-indexing bug — field zones are
allocated sequentially per-field-type across all chunks, so chunks from different archetypes allocate
sequentially: if Chunk A (TVC) allocates PositionX[0..99] and Chunk B (TV) allocates PositionX[100..199],
then Chunk C (TVC) allocates PositionX[200..299] and Color.R[100..199] — entity in Chunk C has PositionX
at global index 200 but Color.R at global index 100. Per-chunk access (via chunk header pointers) is correct;
global-index GPU access is not. The new partition design fixes this because partition regions are pre-allocated
at fixed offsets, not allocated sequentially per archetype.

---

# GPU Upload Architecture

## Cumulative Dirty Bit Array

A dense bit array external to the frame ring, double-buffered:

```
Front: Logic ORs bit atomically (fetch_or, relaxed) as each entity is modified
Back:  Render reads to determine upload set
       Render swaps Front/Back (nanosecond atomic pointer swap) once per frame
```

- Upload set = all entities where Back bit is set; upload their **current state** (not a delta)
- Render holds the slab read lock only during entity state copy (~32µs at 10K dirty entities),
  not during dirty bit coalescing
- Fallback: if `(CurrentLogicFrame - LastGpuUploadFrame) > FrameCount` → full upload all active entities

**When render thread falls behind:** It ORs dirty flags from all intermediate frames before processing:

```
for f in [LastGpuUploadFrame+1 .. CurrentLogicFrame]:
    SIMD OR combinedDirty |= dirtyBits[f % FrameCount]
upload current state for all set bits in combinedDirty
```

This is a SIMD operation on a 12.5KB bit array (100K entities), fits in L1 cache.

## GPU Buffer Design

**5 InstanceBuffers** (for Volatile + Temporal combined entity data):
- Enough depth that the CPU render thread always finds a free buffer without blocking on GPU presentation
- Decouples Logic from VSync: the chain `VSync → GPU holds buffer → CPU render thread blocks →
  holds slab read locks → Logic stalls` is broken by having 5 buffers
- CPU render thread lock on logic slab = only entity state copy duration (~32µs at 10K dirty entities)

GPU interpolation uses its own persistent previous-frame InstanceBuffer. Render thread uploads frame T only
(not T-1). GPU has T-1 from its own last-frame buffer, interpolates in vertex shader.

**Separate static GPU buffer** for static entity data (read-only, uploaded once at initialization).

## Thread Access Summary

| Thread | Reads | Writes |
|--------|-------|--------|
| Logic (Brain) | Frame T (ReadArray) | Frame T+1 (WriteArray) |
| Render (Encoder) | Current logic frame (for GPU upload) | GPU InstanceBuffer |
| GPU | Current InstanceBuffer | Previous InstanceBuffer (for interpolation) |
| Network/Rollback | Historical frames from Temporal slab | Corrected frame (triggers dirty resim) |

---

# Rendering Pipeline

## Raw Vulkan Stack

- **volk 1.4.304** — function loader, vendored at `libs/volk/`
- **VMA 3.3.0** — memory allocator, vendored at `libs/vma/`
- **VulkanContext** — instance, device, swapchain, queues, sync primitives
- **VulkanMemory** — VMA allocator lifetime, buffer/image allocation helpers
- **VulkRender** — Encoder thread: GPU upload, compute dispatch, graphics draw

## GPU-Driven Compute Pipeline (Slang shaders in `shaders/`)

Three-pass compute using shared struct header `GpuFrameData.slang`:

1. **predicate.slang** — reads TemporalFlags (Active + custom predicates), writes `scan[i] = 0 or 1`
2. **prefix_sum.slang** — Option-B scan: subgroup-level prefix, then one `atomicAdd` per workgroup for
   cross-group compaction. No second dispatch.
3. **scatter.slang** — GPU interpolation (lerp between current and previous InstanceBuffer),
   writes compacted InstanceBuffer SoA + `DrawArgs.instanceCount`

The InstanceBuffer is SoA: field `k` (semantic-1), entity `outIdx` →
`InstancesAddr[k * OutFieldStride + outIdx]`.

## cube.vert / cube.frag

- `cube.vert` reads vertex data via Buffer Device Address (`VerticesAddr`), reads instance SoA via
  `InstancesAddr`, reconstructs TRS matrix from Euler-XYZ angles
- ViewProj: column-major `float[16]` from C++ → Slang row-major `float4x4` via `M[r][c] = ViewProj[c*4+r]`
- Projection: right-handed, Y-flip (`m[5] = -F`), depth [0,1]
- `cube.frag` reads interpolated color from SoA

## Compute → Graphics Barrier

```
srcStage  = COMPUTE_SHADER_BIT
dstStage  = VERTEX_SHADER_BIT | DRAW_INDIRECT_BIT
srcAccess = SHADER_WRITE
dstAccess = SHADER_READ | INDIRECT_COMMAND_READ
```

---

# Migration Status

## Implemented (2026-02)

- [x] Three-thread architecture (Sentinel/Brain/Encoder)
- [x] Raw Vulkan: VulkanContext, VulkanMemory (volk + VMA)
- [x] FieldProxy system (Scalar / Wide / WideMask, FieldProxyMask zero-size base)
- [x] Component decomposition into SoA field arrays
- [x] EntityView hydration with zero virtual calls
- [x] TemporalComponentCache dual-buffer (proto-slab)
- [x] Dirty bit tracking (TemporalFlagBits::Active, Dirty)
- [x] Registry dirty bit marking after each chunk update
- [x] LogicThread::PublishCompletedFrame (Vulkan perspective + identity view)
- [x] GPU-driven compute pipeline (predicate → prefix_sum → scatter, Slang)
- [x] VulkRender skeleton (Initialize/Start/Stop/Join wired in)
- [x] Tracy profiler integration (3-level: Coarse/Medium/Fine)

## Designed, Not Yet Implemented

- [ ] **Tiered storage partition layout** (Cold/Static/Volatile/Temporal with DUAL/PHYS/RENDER/LOGIC partitions)
- [ ] **SimulationBody marker component** (Temporal vs Volatile auto-classification)
- [ ] **Universal strip** (contiguous Flags array outside partition field zones)
- [ ] `STRIGID_UNIVERSAL_COMPONENT` macro
- [ ] **SystemGroup tag on STRIGID_TEMPORAL_FIELDS** (drives entity group auto-derivation)
- [ ] **Cumulative dirty bit array** (double-buffered, lock-free Front/Back swap)
- [ ] **5 GPU InstanceBuffers** (VSync decoupling)
- [ ] `GetTemporalFieldWritePtr` migrated from Archetype to TemporalComponentCache
- [ ] `TemporalFrameStride` removed from Archetype (duplicated state — call cache->GetFrameStride())
- [ ] **VulkRender ThreadMain** (GPU loop: upload → compute → draw → present)

## Planned (Next Phase)

- [ ] Jolt Physics integration
- [ ] Frustum culling (SIMD 6-plane test)
- [ ] State-sorted rendering (64-bit sort keys, GPU radix sort)
- [ ] Rollback netcode (Temporal slab rollback + dirty resimulation)
- [ ] Job system (Brain/Encoder currently run inline)

---
