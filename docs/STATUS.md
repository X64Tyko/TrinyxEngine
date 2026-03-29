# Current Status (2026-03)

> **Navigation:** [← Back to README](../README.md) | [← Configuration](CONFIGURATION.md)

---

## Timeline Context

**Project Start:** ~2026-02-01 (Week 1)
**Current Date:** 2026-03-29
**Phase:** Editor (Phase 1–6 of editor plan in progress)

---

## Roadmap

The roadmap is organized into two stages: **Foundation** and **Hardening**. The foundation stage adds the remaining
subsystems needed to build a real game. The hardening stage locks down the substrate before gameplay layer development
begins.

### Stage 1: Foundation (current)

| # | Milestone               | Status      | Notes                                                                                                                   |
|---|-------------------------|-------------|-------------------------------------------------------------------------------------------------------------------------|
| 1 | **Editor (bare-bones)** | In progress | Scene hierarchy, entity inspection, reflected properties, save/load. ImGui docking, 6 panels. JSON serialization.       |
| 2 | **Networking**          | Not started | GNS wrapper, client/server authority model, PIE loopback. Rollback netcode uses existing Temporal slab + Jolt snapshot. |
| 3 | **Audio**               | Not started | SDL3 thin wrapper first (handle-based for Anti-Event compatibility). Minimal — just enough for gameplay feedback.       |

### Stage 2: Hardening

Once Editor + Networking + Audio are functional, the engine enters a dedicated cleanup, refactoring, and rewrite phase.
The goal is to make the substrate as solid as possible before building the gameplay layer (Construct/View system,
behavior trees, etc.) on top of it.

**Hardening targets (non-exhaustive):**

- ~~Wire cumulative dirty bit array to GPU upload~~ ✅ Implemented (2026-03-29)
- Migrate `GetTemporalFieldWritePtr` from Archetype to TemporalComponentCache
- Remove duplicated `TemporalFrameStride` from Archetype
- Fixed-point coordinate system (`Fixed32`, `SimFloat` alias, Jolt bridge validation)
- Audit hot-path data structures for cache efficiency
- Archetype field allocation and meta storage cleanup
- Jolt push logic fixes
- ConstraintEntity system (constraint pool, rigid attachment pass, physics root determination)
- Static entity tier (needs asset importing)
- Evaluate reflection system robustness (static init ordering, precompile step)
- Registration name strings strippable via build option (`TNX_STRIP_NAMES`) for shipping builds

**After hardening:** Arena shooter test level to prove the full stack — high entity counts, physics, competitive input
latency, rollback netcode, networked multiplayer.

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
- Flags read from field slab via `CurrFieldAddrs[0]` (SemFlags=1, always index 0 by convention)
- 14 field semantics: Flags + PosXYZ + RotXYZ + ScaleXYZ + ColorRGBA (SemFlags=1..SemColorA=14)
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

**Dirty Bit Tracking & Selective GPU Upload:**

- FieldProxy sets dirty bits (bit 30 + bit 29) on every field write
- DirtiedFrame (bit 29): per-frame flag, cleared unconditionally at frame start
- Dirty (bit 30): accumulates until render acknowledges via RenderAck atomic handshake
- 5 heap-allocated GPU dirty bitplanes (one per field slab) — AVX2 scan of slab Flags, OR into all planes
- Selective scatter: only dirty entities uploaded per field per frame (ctzll bit iteration)
- FirstSlabWrite: full copy on first write to each slab slot to bootstrap GPU state
- PullActiveTransforms manually marks dirty bits after Jolt writeback (bypasses FieldProxy)

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
- JoltBody volatile component (TNX_VOLATILE_FIELDS, CacheTier::Volatile) — SoA in slab, slab-direct iteration
- FlushPendingBodies: iterates contiguous DUAL+PHYS slab region directly via GetPhysicsRange(), no archetype/chunk
  indirection
- PullActiveTransforms: writes pos+rot from awake bodies back into SoA WriteArrays
- Physics loop: PrePhysics → (FlushPendingBodies → Jolt Step -- phystick % 0) → (PullActiveTransforms -- phystick %
  phystick - 1) → PostPhysics
- BodyID↔EntityCacheIndex bidirectional lookup (EntityToBody, BodyToEntity vectors)
- GetPhysicsRange() on ComponentCacheBase: returns [DualStart, PhysEnd) in cache index units for dense physics iteration

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

| Test                                   | Entity Count | Time                            | Notes                                   |
|----------------------------------------|--------------|---------------------------------|-----------------------------------------|
| PrePhysics (Transform only)            | 100k         | ~0.1ms                          | On target                               |
| Full Frame (no physics)                | 100k         | ~0.3ms with propagation         | Well under 1.95ms budget                |
| 15-layer pyramid (1,240 cubes)         | 1,240        | avg 1ms (capped 1024 FPS)       | 58μs physics, 105μs on Jolt pull frames |
| 25-layer pyramid (5,525 cubes)         | 5,526        | avg ~1ms, spike 14.67ms         | Slab-direct iteration, 64Hz physics     |
| 100k cubes + 25-layer pyramid          | 105,526      | 0.73ms steady, 18.74ms settling | 1375 FPS steady, 53 FPS settling        |
| 205k entities (100k super + 5.5k phys) | 205,526      | ~1.4ms steady, 28ms settling    | 512Hz maintained throughout             |
| Jolt step (15-layer)                   | 1,240        | 4.13ms                          | 512Hz logic / 8 lockstep = 64Hz physics |
| Jolt step (25-layer)                   | 5,526        | ~12ms settling, <1ms steady     | Monolithic island during settling phase |

### Encoder (Render Thread)

| Test                             | Time    | FPS  | Notes                                       |
|----------------------------------|---------|------|---------------------------------------------|
| 100k cubes + 25-layer pyramid    | ~0.88ms | 1133 | Dirty-bit selective upload, steady state    |
| 205k entities (100k + 5.5k phys) | ~1.5ms  | 660  | Steady state after physics settles          |
| 205k entities (settling)         | ~3.1ms  | 320  | All 5.5k physics entities dirty every frame |

### Input-to-Photon Latency

| Test                          | Avg    | Max    | Notes                           |
|-------------------------------|--------|--------|---------------------------------|
| 100k cubes + 25-layer pyramid | 9.24ms | —      | 240Hz monitor, dirty-bit upload |
| 205k entities (settling)      | 14.3ms | 16.8ms | Under heavy physics load        |
| 205k entities (steady)        | 9.1ms  | —      | Physics mostly asleep           |

---

## Architecture Status

### Implemented

- [x] Three-thread architecture (Sentinel / Brain / Encoder)
- [x] Raw Vulkan: VulkanContext, VulkanMemory (volk + VMA)
- [x] FieldProxy (Scalar / Wide / WideMask, FieldProxyMask zero-size base)
- [x] TemporalComponentCache dual-buffer SoA (proto-slab)
- [x] Dirty bit tracking (Active = 1<<31, Dirty = 1<<30, DirtiedFrame = 1<<29)
- [x] Registry dirty bit marking after each chunk update
- [x] Dirty-bit-driven selective GPU upload (RenderAck handshake, per-slab bitplanes, AVX2 scan)
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
- [x] Jolt Physics v5.5.0 (JoltJobSystemAdapter, JoltBody volatile component, slab-direct
  FlushPendingBodies/PullActiveTransforms)
- [x] Cold component infrastructure (TNX_REGISTER_FIELDS → CacheTier::None → SoA in chunk)
- [x] Input buffering (double-buffered, lock-free polling, event + bitstate querying)
- [x] VecMath / QuatMath / FieldMath libraries
- [x] Component validation: no vtable + all fields must be FieldProxy (SchemaValidation.h)

### Designed, Not Yet Implemented

- [ ] **Construct/View OOP layer** — `Construct<T>` (CRTP lifecycle owner), `Owned<T>` (composition), `ConstructBatch` (
  type-erased tick dispatch), `TickGroup` enum, View family (Instance/Phys/Render/Logic), defrag listeners, spawn
  handshake integration for registration
- [x] **Dirty-bit selective GPU upload** — RenderAck handshake, 5 dirty bitplanes, AVX2 scan + ctzll scatter
- [ ] **Fixed-point coordinate system** — `Fixed32` value type (1 unit = 0.1mm), `FieldProxy<Fixed32, WIDTH>`, render thread conversion to camera-relative float32 at upload, Jolt bridge (int32↔float32 at cell boundary)
- [ ] **ConstraintEntity system** — constraint entities in LOGIC partition, `ConstraintType` enum (Rigid/Hinge/BallSocket/Prismatic/Distance/Spring), render thread rigid attachment pass (2-pass depth ordering), physics root determination
- [ ] **Space partition cell registry** — cell world origins as float64/int64, cell assignment at entity spawn, cross-cell reparenting
- [ ] `GetTemporalFieldWritePtr` migration from Archetype to TemporalComponentCache
- [ ] `TemporalFrameStride` removal from Archetype (duplicated from cache)

### Known Issues / Technical Debt

1. **TemporalFrameStride duplicated on Archetype:** Should call `cache->GetFrameStride()` instead.
   Currently cached redundantly on every Archetype.

### Planned (Next Phase)

- [x] **Render pipeline optimization** — dirty-bit selective GPU upload (2026-03-29)
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
