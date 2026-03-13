# Current Status (2026-03)

> **Navigation:** [← Back to README](../README.md) | [← Configuration](CONFIGURATION.md)

---

## Timeline Context

**Project Start:** ~2026-02-01 (Week 1)
**Current Date:** 2026-03-12
**Phase:** Physics Integration + Render Pipeline Optimization

---

## Completed Work

### Foundation & Trinity Architecture

**Threading Model:**
- Trinyx Trinity (Sentinel/Brain/Encoder) fully operational
- Sentinel runs 1000Hz SDL event loop, owns VulkanContext + VulkanMemory
- Brain runs 512Hz fixed-timestep loop via accumulator (std::atomic<double>)
- Encoder (VulkRender) functional — full GPU loop rendering entity data from TemporalComponentCache

**Raw Vulkan Stack:**
- Migrated from SDL3 GPU backend to raw Vulkan (volk 1.4.304 + VMA 3.3.0)
- VulkanContext + VulkanMemory migrated to vk::raii::
- VulkanContext: instance, device, swapchain, queues, sync primitives
- VulkanMemory: VMA allocator lifetime, buffer/image allocation helpers
- VulkRender Steps 1–4 complete: clear → indexed cube pipeline → GpuFrameData + BDA draw → entity data from
  TemporalComponentCache
  Live entity data rendering at full rate via Buffer Device Address pipeline

**GPU-Driven Compute Pipeline (Slang):**
- 5 shaders in `shaders/`: predicate, prefix_sum, scatter, cube.vert, cube.frag
- Shared struct header: `shaders/GpuFrameData.slang` (C++ mirror: `GpuFrameData.h`, static_assert 3192 bytes)
- CMakeLists: slangc invocations with `-I shaders` + GpuFrameData.slang in DEPENDS
- 3-pass: predicate → prefix_sum (Option-B subgroup scan) → scatter (GPU interpolation + InstanceBuffer)
- Flags read from field slab via `CurrFieldAddrs[0]` (kSemFlags=1, always index 0 by convention)
- 14 field semantics: Flags + PosXYZ + RotXYZ + ScaleXYZ + ColorRGBA (kSemFlags=1..kSemColorA=14)
- 5 field slabs (PersistentMapped, cycling independently of 2 GPU frame slots)
- Compute→graphics barrier: `dstStage = VERTEX_SHADER_BIT | DRAW_INDIRECT_BIT`
- Camera wired: LogicThread::PublishCompletedFrame writes Vulkan RH perspective + identity view each frame
- Testbed: 10k CubeEntity + 90k SuperCube at Z=-200..-500 (visible as colored specks at z=-200)

**ECS Architecture:**

- Archetype-based ECS with adjustable EntitiesPerChunk defined in entity classes.
- FieldProxy system: Scalar / Wide / WideMask modes
- FieldProxyMask<WIDTH> zero-size base (Scalar mode saves 32 bytes/field)
- TemporalComponentCache: N-frame buffer SoA
- EntityView hydration (zero virtual calls, schema-driven)
- SIMD batch processing (AVX2), 3-level Tracy profiling (Coarse/Medium/Fine)

**Dirty Bit Tracking:**

- DirtyFlag bitplane in the registry.
- FieldProxy manages setting dirty bits any time a value is modified.

**Reflection System:**
- `TNX_REGISTER_COMPONENT(T)` — component type registration
- `TNX_TEMPORAL_FIELDS(T, SystemGroup, ...)` — SoA temporal field decomposition
- `TNX_REGISTER_FIELDS(T, ...)` — cold (chunk-only) field registration
- `TNX_REGISTER_SCHEMA / TNX_REGISTER_SUPER_SCHEMA` — entity schema + Advance generation

**Worth Noting:** This is entirely template and macro driven at the moment, it is potentially fragile
and relies on static initialization. A dedicated precompile or something like UBT would be a good idea
in the long run.

**Job System & Threading:**

- Lock-free MPMC job system (Vyukov ring buffers, 4 queues: Physics/Render/Logic/General)
- Futex-based worker wake (`std::atomic::wait`/`notify` — zero idle CPU, ~1-2μs wake latency)
- Core-aware thread pinning (physical cores first, SMT siblings second, core 0 skipped)
- Brain/Encoder act as coordinators + workers (dispatch jobs, then steal work while waiting)
- 25% workers dedicated to Jolt physics queue by default
- GameManager CRTP pattern (`TNX_IMPLEMENT_GAME` macro — zero-boilerplate project setup)
- Project-relative INI config (`*Defaults.ini` scanning from source directory via `TNX_PROJECT_DIR`)

**Tiered Storage:**

- Dual-ended arena partition layout (4 tiers: Cold/Static/Volatile/Temporal)
- 4 partitions within each slab: Dual/Phys/Render/Logic (auto-derived from SystemGroup tags)

**Jolt Physics (v5.5.0):**

- JoltJobSystemAdapter bridges JPH::JobSystemWithBarrier onto Jolt job queue
- JoltBody cold component (TNX_REGISTER_FIELDS, CacheTier::None) — SoA in chunk, not slab
- FlushPendingBodies: creates Jolt bodies for unmapped entities on spawn
- PullActiveTransforms: writes pos+rot from awake bodies back into SoA WriteArrays
- Physics loop: PrePhysics → (FlushPendingBodies → Jolt Step -- phystick % 0) → (PullActiveTransforms -- phystick %
  phystick - 1) → PostPhysics
- BodyID↔EntityGlobalIndex bidirectional lookup (EntityToBody, BodyToEntity vectors)

**Input System:**

- Double-buffered input with lock-free polling on Sentinel thread (1000Hz)
- Event buffer + bitstate for flexible querying (event-driven or polled)

**Math Libraries:**

- VecMath.h: VecLocal<N,WIDTH>, Vec2/3/4Accessor (reference-based, embedded in components)
- QuatMath.h: QuatLocal<WIDTH>, QuatAccessor
- FieldMath.h: Read, Clamp, Min, Max, Abs, Lerp, Step, SmoothStep

**Architecture & Config Fixes:**
- `MAX_FIELDS_PER_ARCHETYPE = 256` in Types.h
- `DualArrayTableBuffer[MAX_FIELDS_PER_ARCHETYPE * 2]` moved from 256 KB stack to Registry member
- `Archetype::BuildFieldArrayTable` with dual-pointer interleave
- `Archetype::GetTemporalFieldWritePtr` (note: should eventually migrate to TemporalComponentCache)
- VulkanContext::CreateInstance suppresses unused `window` param with `/*window*/`
- Tracy TRACY_SOURCES compiled with `-w` (suppress all upstream warnings)

---

## Performance Metrics (RelWithDebInfo, Tracy)

### Sentinel (Main Thread)

| Task          | Time   | Notes                                 |
|---------------|--------|---------------------------------------|
| Full Frame    | 1.0ms  | ✅ 1000Hz target achieved              |
| Event Polling | 0.1ms  | SDL3, lock-free double-buffered input |
| Frame Pacing  | ~0.8ms | Sleep + busy-wait tail                |

### Brain (Logic Thread, 512Hz)

| Test                           | Entity Count | Time                      | Notes                                      |
|--------------------------------|--------------|---------------------------|--------------------------------------------|
| PrePhysics (Transform only)    | 100k         | ~0.1ms                    | On target                                  |
| Full Frame (no physics)        | 100k         | ~0.3ms with propagation   | Well under 1.95ms budget                   |
| 15-layer pyramid (1,240 cubes) | 1,240        | avg 1ms (capped 1024 FPS) | 58μs physics, 105μs on Jolt pull frames    |
| 25-layer pyramid (5,525 cubes) | 5,525        | avg 1ms, max 15.58ms      | 1.16ms physics, 2.44ms on Jolt pull frames |
| Jolt step (15-layer)           | 1,240        | 4.13ms                    | 512Hz logic / 8 lockstep = 64Hz physics    |
| Jolt step (25-layer)           | 5,525        | 12.58ms                   | 512Hz logic / 8 lockstep = 64Hz physics    |

### Encoder (Render Thread)

| Task       | Time    | Notes                         |
|------------|---------|-------------------------------|
| Full Frame | ~0.73ms | 100k entities, no culling yet |

### Input-to-Photon Latency

| Test             | Avg     | Max     | Notes         |
|------------------|---------|---------|---------------|
| 15-layer pyramid | 7.37ms  | —       | 240Hz monitor |
| 25-layer pyramid | 13.25ms | 18.08ms | Under load    |

---

## Architecture Status

### Implemented

- [x] Three-thread architecture (Sentinel / Brain / Encoder)
- [x] Raw Vulkan: VulkanContext, VulkanMemory (volk + VMA)
- [x] FieldProxy (Scalar / Wide / WideMask, FieldProxyMask zero-size base)
- [x] TemporalComponentCache dual-buffer SoA (proto-slab)
- [x] Dirty bit tracking (Active = 1<<31, Dirty = 1<<30)
- [x] Registry dirty bit marking after each chunk update
- [x] LogicThread::PublishCompletedFrame (Vulkan RH perspective + identity view)
- [x] GPU-driven compute pipeline (predicate → prefix_sum → scatter, Slang shaders)
- [x] InstanceBuffer SoA + indirect draw (DrawArgs)
- [x] VulkRender Steps 1–4: clear → indexed cube → GpuFrameData + BDA draw → entity data from TemporalComponentCache
- [x] 3-level Tracy profiling (Coarse/Medium/Fine)
- [x] `TNX_TEMPORAL_FIELDS` with SystemGroup tag (drives entity group auto-derivation)
- [x] `TemporalFlags` with Active/Dirty bits
- [x] Lock-free job system (MPMC ring buffers, futex-based wake, per-chunk dispatch)
- [x] Core-aware thread pinning (physical cores first, SMT siblings second)
- [x] GameManager CRTP pattern (`TNX_IMPLEMENT_GAME` macro)
- [x] Project-relative INI config (`*Defaults.ini` scanning from source directory)
- [x] Tiered storage partition layout (Cold/Static/Volatile/Temporal with dual-ended arena layout)
- [x] 5 GPU InstanceBuffers (circular buffer while rGPU compute pipeline is in progress)
- [x] Jolt Physics v5.5.0 (JoltJobSystemAdapter, JoltBody cold component, FlushPendingBodies/PullActiveTransforms)
- [x] Cold component infrastructure (TNX_REGISTER_FIELDS → CacheTier::None → SoA in chunk)
- [x] Input buffering (double-buffered, lock-free polling, event + bitstate querying)
- [x] VecMath / QuatMath / FieldMath libraries
- [x] Component validation: no vtable + all fields must be FieldProxy (SchemaValidation.h)

### Designed, Not Yet Implemented

- [ ] **Cumulative dirty bit array** — tracking functional, not yet wired to GPU upload path
- [ ] **Fixed-point coordinate system** — `Fixed32` value type (1 unit = 0.1mm), `FieldProxy<Fixed32, WIDTH>`, render thread conversion to camera-relative float32 at upload, Jolt bridge (int32↔float32 at cell boundary)
- [ ] **ConstraintEntity system** — constraint entities in LOGIC partition, `ConstraintType` enum (Rigid/Hinge/BallSocket/Prismatic/Distance/Spring), render thread rigid attachment pass (2-pass depth ordering), physics root determination
- [ ] **Space partition cell registry** — cell world origins as float64/int64, cell assignment at entity spawn, cross-cell reparenting
- [ ] `GetTemporalFieldWritePtr` migration from Archetype to TemporalComponentCache
- [ ] `TemporalFrameStride` removal from Archetype (duplicated from cache)

### Known Issues / Technical Debt

2. **TemporalFrameStride duplicated on Archetype:** Should call `cache->GetFrameStride()` instead.
   Currently cached redundantly on every Archetype.

### Planned (Next Phase)

- [ ] **Render pipeline optimization** — wire dirty bits to GPU upload
- [ ] **Frustum culling** — SIMD 6-plane test, GPU-side predicate enhancement
- [ ] **State-sorted rendering** — 64-bit sort keys, GPU radix sort after scatter
- [ ] **Rollback netcode** — Temporal slab rollback + dirty resimulation

---

## Design Insights Log

- **Volatile = 3 frames** ~~(not 4): 5 frames gives Logic/4 headroom between thread tick rates.
  4 frames = Logic/2, which is tighter margin for the render thread to find a safe read frame.~~

    - With the update to GPU driven rendering and frame propagation, logic and render threads now only need
      1 frame each to work on and wont be blocked with a triple buffer instead of needing 5.

- **GPU interpolation is self-contained:** Render thread uploads frame T only. GPU keeps T-1 in its
  own persistent previous-frame InstanceBuffer. Render thread does NOT need to read two CPU slab frames.

- **5 GPU InstanceBuffers break the VSync chain:** Without 5 buffers, VSync → GPU holds buffer →
  CPU render thread blocks → holds slab read lock → Logic thread stalls. 5 buffers decouple this
  entirely. If render falls below Logic/4, it's a renderer performance problem, not a sync problem.

- **Cumulative dirty bit array (SIMD OR path):** When render falls behind multiple logic frames,
  it ORs dirty flags from all intermediate frames (SIMD, 12.5KB for 100K entities, fits in L1)
  before building the GPU upload set. No full upload required unless lag exceeds ring buffer depth.

- **Dirty bit = rollback blast radius:** The same bit 30 that drives GPU upload drives selective
  resimulation during rollback. The dirty set from a correction expands naturally through update
  logic to the exact set of entities that need recomputation — no manual dependency tracking.

---
