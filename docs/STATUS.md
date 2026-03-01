# Current Status (2026-02)

> **Navigation:** [← Back to README](../README.md) | [← Configuration](CONFIGURATION.md)

---

## Timeline Context

**Project Start:** ~2026-02-01 (Week 1)
**Current Date:** 2026-03-01
**Phase:** GPU-Driven Pipeline + Tiered Storage Design

---

## Completed Work

### Foundation & Trinity Architecture

**Threading Model:**
- Strigid Trinity (Sentinel/Brain/Encoder) fully operational
- Sentinel runs 1000Hz SDL event loop, owns VulkanContext + VulkanMemory
- Brain runs 512Hz fixed-timestep loop via accumulator (std::atomic<double>)
- Encoder (VulkRender) skeleton wired in — ThreadMain logs and exits (GPU loop in progress)

**Raw Vulkan Stack:**
- Migrated from SDL3 GPU backend to raw Vulkan (volk 1.4.304 + VMA 3.3.0)
- VulkanContext: instance, device, swapchain, queues, sync primitives
- VulkanMemory: VMA allocator lifetime, buffer/image allocation helpers
- VulkRender: Initialize/Start/Stop/Join wired into StrigidEngine

**GPU-Driven Compute Pipeline (Slang):**
- 5 shaders in `shaders/`: predicate, prefix_sum, scatter, cube.vert, cube.frag
- Shared struct header: `shaders/GpuFrameData.slang`
- CMakeLists: slangc invocations with `-I shaders` + GpuFrameData.slang in DEPENDS
- 3-pass: predicate → prefix_sum (Option-B subgroup scan) → scatter (GPU interpolation + InstanceBuffer)
- Compute→graphics barrier: `dstStage = VERTEX_SHADER_BIT | DRAW_INDIRECT_BIT`
- Camera wired: LogicThread::PublishCompletedFrame writes Vulkan RH perspective + identity view each frame
- Testbed: 10k CubeEntity + 90k SuperCube at Z=-200..-500 (visible as colored specks at z=-200)

**ECS Architecture:**
- Archetype-based ECS with 64 KB chunks
- FieldProxy system: Scalar / Wide / WideMask modes
- FieldProxyMask<WIDTH> zero-size base (Scalar mode saves 32 bytes/field)
- TemporalComponentCache: dual-buffer SoA (ReadArray/WriteArray per field, per archetype)
- EntityView hydration (zero virtual calls, schema-driven)
- SIMD batch processing (AVX2), 3-level Tracy profiling (Coarse/Medium/Fine)

**Dirty Bit Tracking:**
- `TemporalFlagBits::Active = 1<<31`, `Dirty = 1<<30`
- Registry::InvokePrePhys/PostPhys/ScalarUpdate OR bit 30 into write-frame flags after each chunk update

**Reflection System:**
- `STRIGID_REGISTER_COMPONENT(T)` — component type registration
- `STRIGID_TEMPORAL_FIELDS(T, SystemGroup, ...)` — SoA temporal field decomposition
- `STRIGID_REGISTER_FIELDS(T, ...)` — cold (chunk-only) field registration
- `STRIGID_REGISTER_SCHEMA / STRIGID_REGISTER_SUPER_SCHEMA` — entity schema + Advance generation

**Architecture & Config Fixes:**
- `MAX_FIELDS_PER_ARCHETYPE = 256` in Types.h
- `DualArrayTableBuffer[MAX_FIELDS_PER_ARCHETYPE * 2]` moved from 256 KB stack to Registry member
- `Archetype::BuildFieldArrayTable` with dual-pointer interleave
- `Archetype::GetTemporalFieldWritePtr` (note: should eventually migrate to TemporalComponentCache)
- VulkanContext::CreateInstance suppresses unused `window` param with `/*window*/`
- Tracy TRACY_SOURCES compiled with `-w` (suppress all upstream warnings)

---

## Performance Metrics (RelWithDebInfo, Tracy)

### Logic Thread (128Hz Fixed Update)

| Test | Entity Count | Time | Notes |
|------|-------------|------|-------|
| PrePhysics (Transform only) | 10k | 0.03ms | Well under budget |
| PrePhysics (Transform only) | 100k | 0.30ms | On track for 512Hz |
| Full Frame (PrePhys + overhead) | 100k | 1.7ms | Includes ECS dispatch, Tracy |

### Main Thread

| Task | Time | Notes |
|------|------|-------|
| Full Frame | 1.0ms | ✅ 1000Hz target achieved |
| Event Polling | 0.1ms | SDL3 |
| Frame Pacing | ~0.8ms | Sleep + busy-wait tail |

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
- [x] VulkRender skeleton (Initialize/Start/Stop/Join)
- [x] 3-level Tracy profiling (Coarse/Medium/Fine)
- [x] `STRIGID_TEMPORAL_FIELDS` with SystemGroup tag (syntax implemented, partition routing pending)
- [x] `TemporalFlags` with Active/Dirty bits

### Designed, Not Yet Implemented

- [ ] **Tiered storage partition layout** — Cold/Static/Volatile/Temporal with DUAL/PHYS/RENDER/LOGIC partitions
  (current: all temporal entities share one TemporalComponentCache, no partition isolation)
- [ ] **SimulationBody marker component** — Temporal vs Volatile explicit opt-in
- [ ] **Universal strip** — contiguous Flags array outside partition field zones
- [ ] `STRIGID_UNIVERSAL_COMPONENT` macro
- [ ] **Cumulative dirty bit array** — double-buffered dense bit array, lock-free Front/Back swap
- [ ] **5 GPU InstanceBuffers** — VSync decoupling (currently fewer buffers in flight)
- [ ] **Fixed-point coordinate system** — `Fixed32` value type (1 unit = 0.1mm), `FieldProxy<Fixed32, WIDTH>`, render thread conversion to camera-relative float32 at upload, Jolt bridge (int32↔float32 at cell boundary)
- [ ] **ConstraintEntity system** — constraint entities in LOGIC partition, `ConstraintType` enum (Rigid/Hinge/BallSocket/Prismatic/Distance/Spring), render thread rigid attachment pass (2-pass depth ordering), physics root determination
- [ ] **Space partition cell registry** — cell world origins as float64/int64, cell assignment at entity spawn, cross-cell reparenting
- [ ] `GetTemporalFieldWritePtr` migration from Archetype to TemporalComponentCache
- [ ] `TemporalFrameStride` removal from Archetype (duplicated from cache)
- [ ] **VulkRender ThreadMain** — actual GPU loop (acquire → upload → compute → draw → present)

### Known Issues / Technical Debt

1. **Cross-archetype co-indexing bug:** TemporalComponentCache allocates field zones sequentially
   per-field-type across all chunks. Entity in Chunk C (TVC) may have PositionX at global index 200
   but Color.R at global index 100. Per-chunk access is correct; global-index GPU access is not.
   New partition design fixes this.

2. **TemporalFrameStride duplicated on Archetype:** Should call `cache->GetFrameStride()` instead.
   Currently cached redundantly on every Archetype.

3. **VulkRender::ThreadMain is a stub:** Logs "not yet implemented" and exits. Engine runs with
   the logic thread functional and render thread placeholder.

### Planned (Next Phase)

- [ ] **VulkRender::ThreadMain** — GPU loop: acquire swapchain → upload dirty → compute dispatch → draw → present
- [ ] **Tiered slab implementation** — allocate Volatile + Temporal slabs with partition layout
- [ ] **Frustum culling** — SIMD 6-plane test, GPU-side predicate enhancement
- [ ] **State-sorted rendering** — 64-bit sort keys, GPU radix sort after scatter
- [ ] **Jolt Physics integration** — zero-copy RigidBody mapping
- [ ] **Rollback netcode** — Temporal slab rollback + dirty resimulation
- [ ] **Job system** — Brain/Encoder currently run update loops inline; job infrastructure planned
- [ ] **Input system** — 1000Hz sampling, action maps

---

## Design Insights Log

- **Volatile = 5 frames** (not 4): 5 frames gives Logic/4 headroom between thread tick rates.
  4 frames = Logic/2, which is tighter margin for the render thread to find a safe read frame.

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
