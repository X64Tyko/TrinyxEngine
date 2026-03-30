# Performance Targets

> **Navigation:** [← Back to README](../README.md) | [← Architecture](ARCHITECTURE.md) | [Data Structures →](DATA_STRUCTURES.md)

---

## Primary Target: 100k Dynamic Entities @ 512Hz

The engine is designed for high-frequency simulation with a large number of dynamic entities. Performance is measured in **milliseconds per frame**, not FPS.

### Core Performance Budget (512Hz = 1.95ms per frame)

| Phase                  | Budget     | Target                                       | Status      |
|------------------------|------------|----------------------------------------------|-------------|
| **PrePhysics**         | 0.4ms      | User logic, input processing, AI decisions   | 🎯 Target   |
| **Physics Simulation** | 0.8ms      | Jolt solver, collision detection, response   | 🎯 Target   |
| **PostPhysics**        | 0.3ms      | Collision callbacks, state updates           | 🎯 Target   |
| **History Write**      | 0.2ms      | Write to History Slab section, update header | 🎯 Target   |
| **Overhead**           | 0.25ms     | Scheduling, atomics, profiling               | 🎯 Tracking |
| **TOTAL**              | **1.95ms** | **Full simulation frame @ 512Hz**            | 🎯 Target   |

**Current Reality (Week 8, 512Hz):**

- PrePhysics: ~1.0ms for 1M entities (stress test)
- PrePhysics: ~0.1ms for 100k entities (on target)
- Jolt solver at 64Hz (512Hz/8 ratio), parallelized across worker pool via JoltJobSystemAdapter
- 25-layer pyramid (5,526 entities): ~1ms steady-state, 14.67ms settling spike. Slab-direct iteration (no archetype
  indirection)
- 100k cubes + 25-layer pyramid: Logic 0.73ms steady-state (1375 FPS), 18.74ms during settling
- History Write--Frame Propagation: ~0.2ms for 100k entities.

---

## Render Thread Performance

Target: 60-120 FPS (8-16ms per frame) for 100k visible entities

| Phase                       | Budget    | Description                            | Status         |
|-----------------------------|-----------|----------------------------------------|----------------|
| **History Access**          | 0.5ms     | Read T-1 and T sections from slab      | 🎯 Target      |
| **Culling + Interpolation** | 3.0ms     | Frustum cull, lerp, build InterpBuffer | 🎯 Target      |
| **State Sorting**           | 1.0ms     | Sort InterpBuffer by 64-bit keys       | ⏳ Pending      |
| **GPU Upload**              | 1.5ms     | Transfer buffer write, copy commands   | ✅ Working      |
| **Command Encoding**        | 2.0ms     | Build render pass, draw calls          | ✅ Working      |
| **GPU Submit + Sync**       | 0.5ms     | Frame fence, swapchain acquisition     | ✅ Working      |
| **TOTAL**                   | **8.5ms** | **~117 FPS rendering budget**          | 🔄 In Progress |

**Current Reality (Week 8):**

- Full frame: ~0.88ms (1133 FPS) for 100k entities with dirty-bit selective upload
- 205k entities (100k super cubes + 5.5k physics): ~3.1ms (320 FPS) during settling, ~1.5ms (660 FPS) steady-state
- No culling yet (rendering all entities)
- No state sorting yet (naive draw order)
- Dirty-bit-driven GPU upload operational — only modified entities uploaded per frame

---

## Scalability Targets

### Entity Count vs Performance

| Entity Count | PrePhysics (512Hz) | Render (60 FPS) | Notes               |
|--------------|--------------------|-----------------|---------------------|
| 10k          | 0.01ms             | 0.8ms           | ✅ Trivial           |
| 50k          | 0.08ms             | 4.0ms           | ✅ Comfortable       |
| 100k         | 0.15ms             | 8.0ms           | 🎯 Primary Target   |
| 250k         | 0.37ms             | 20ms (50 FPS)   | 🎯 Stretch Goal     |
| 1M           | 1.0ms              | 80ms (12 FPS)   | 🔬 Stress Test Only |

**Notes:**
- 1M entities is a **benchmark**, not a production target
- With physics, 100k is ambitious; 50k may be more realistic
- Culling will dramatically reduce render times for large scenes
---

## Thread Timing Targets

### Sentinel (Main Thread)

Target: 1000Hz (1.0ms per iteration)

| Task          | Budget    | Notes                         |
|---------------|-----------|-------------------------------|
| Event Polling | 0.1ms     | SDL_PollEvent, input sampling |
| Idle/Timing   | 0.9ms     | Sleep to maintain 1000Hz      |
| **TOTAL**     | **1.0ms** | **1000Hz main loop**          |

Status: ✅ Currently running at ~1.0ms (1000 Hz) - hitting target exactly

### Brain (Logic Thread)

Target: 512Hz (1.95ms per iteration)

- See "Core Performance Budget" table above
- Currently runs at 512Hz with 100k non-physics entities

### Encoder (Render Thread)

Target: 60-120 FPS (8-16ms per frame)

- See "Render Thread Performance" table above
- Currently runs at 1000 FPS (~0.7ms per frame)
- GPU isn't being taxed at all by instanced cubes, locked to VSync

---

## Memory Targets

### Per-Entity Memory Footprint (Physics-Enabled)

Assumes 100k entities with 90/10 split between simple and complex entities:

**Hot Component Sizes:**
- **Simple Entity:** Transform (36B) + Velocity (24B) + Forces (24B) + Collider (32B) = **116 bytes**
- **Complex Entity:** Transform (36B) + Velocity (24B) + Forces (24B) + Collider (32B) + BoneArray/Extra (200B) = **316 bytes**

**Per-Frame Base (100k mixed entities):**
- Simple (90k): 116 bytes × 90,000 = 10.44 MB
- Complex (10k): 316 bytes × 10,000 = 3.16 MB
- **Total per-frame:** 13.6 MB

| Component Type               | 100k Mixed | 100k Simple Only | 100k Complex Only |
|------------------------------|------------|------------------|-------------------|
| **Hot per-frame**            | 13.6 MB    | 11.6 MB          | 31.6 MB           |
| **Hot × 128 pages**          | 1.74 GB    | 1.48 GB          | 4.04 GB           |
| **Cold Components**          | 50 MB      | 50 MB            | 50 MB             |
| **ECS Metadata (~5%)**       | 100 MB     | 80 MB            | 220 MB            |
| **Total (128 pages)**        | **1.89 GB**| **1.61 GB**      | **4.31 GB**       |

**Notes:**
- Hot components (Transform, Velocity, Forces, Collider) stored in History Slab with full history
- Cold components (AI state, inventory, health) stored in archetype chunks without history
- 90/10 simple/complex split is realistic (projectiles, debris vs characters, vehicles)
- Page count directly scales memory usage

### Reasonable Configurations

| Config                     | 100k Mixed | 250k Mixed | Notes                     |
|----------------------------|------------|------------|---------------------------|
| 4 pages (render only)      | 104 MB     | 260 MB     | ✅ Lightweight, interpolation only |
| 16 pages (rollback)        | 317 MB     | 792 MB     | ✅ Good for netcode, 0.125s @ 128Hz |
| 64 pages (replay)          | 970 MB     | 2.42 GB    | 🎯 Extended history, 0.5s @ 128Hz |
| 128 pages (full history)   | 1.89 GB    | 4.72 GB    | ⚠️ High memory, 1.0s @ 128Hz or 0.25s @ 512Hz |

**Target:** Support 100k entities with 128-page history on 8GB RAM systems

---

## Latency Targets

### Input-to-Photon Latency

Target: <16ms (one frame @ 60Hz)

| Stage                     | Latency Avg. | Notes                                                  |
|---------------------------|--------------|--------------------------------------------------------|
| Input Sampling (Sentinel) | 0.5ms        | 1000Hz polling, immediate capture                      |
| Logic Processing (Brain)  | 1.95ms       | One 512Hz tick to process input                        |
| Render Frame (Encoder)    | 0.7ms        | GPU BDA push                                           |
| GPU Execution             | 16.6ms       | Actual rendering on GPU + VSync, assuming 60Hz monitor |
| **TOTAL (best case)**     | **18.75ms**  | ✅ Under one 60Hz frame                                 |
| **TOTAL (worst case)**    | ~19.75ms     | If input arrives just after logic tick                 |

**Status:** Averaging ~7ms on 240Hz monitor. Even when Logic falls under 30 FPS seeing ~20ms.

---

## Network Performance Targets

Target: 30Hz network tick (33ms per packet)

| Metric                  | Target        | Notes                                      |
|-------------------------|---------------|--------------------------------------------|
| Packet Send Rate        | 30 Hz         | Configurable via `NetworkUpdateHz`         |
| Rollback Window         | 128 frames    | 0.25s @ 512Hz, 1.0s @ 128Hz               |
| Resim Time              | <5ms          | Rollback + resimulate 128 frames           |
| Delta Compression       | 70-90%        | XOR delta vs last acknowledged state       |
| Prediction Accuracy     | >95%          | Client matches server within tolerance     |

**Status:** Rollback substrate proven. Resim benchmarked at ~18ms for ~5 frames (100k entities + 56 physics bodies), no
dirty propagation optimization yet. Byte-perfect determinism confirmed for both ECS and Jolt physics (2026-03-29).

**Rollback Benchmark (2026-03-29):**

| Metric                           | Result       | Notes                                             |
|----------------------------------|--------------|---------------------------------------------------|
| Resim (5 frames, 100k entities)  | ~18ms        | Full frame resim, no dirty propagation            |
| Resim (12 frames, 100k entities) | ~28ms        | Crossing 2 physics boundaries                     |
| Jolt RestoreSnapshot             | <1ms         | 7KB state via StateRecorderImpl                   |
| ECS determinism                  | Byte-perfect | 34MB field data, verified via memcmp              |
| Jolt determinism                 | Byte-perfect | 7,436 bytes, CROSS_PLATFORM_DETERMINISTIC enabled |
| Per-frame SaveSnapshot overhead  | <0.1ms       | 7KB serialize after each PullActiveTransforms     |

Full frame resim includes PrePhysics + Physics Step + FlushPendingBodies + PullActiveTransforms + PostPhysics +
PropagateFrame for every frame in the rollback window. Dirty propagation (resimulating only corrected entities) is
designed but not yet wired — expected to reduce resim cost by 10-100x for typical corrections affecting <1% of entities.

---

## Benchmark Methodology

All benchmarks performed on:
- CPU: [Intel Core Ultra 9 275HX, AMD Ryzen 9 9950X]
- RAM: [32GB, 64GB]
- GPU: [RTX 5070 Ti, RTX 4070]
- OS: CachyOS Arch Linux, Windows 11
- Build: RelWithDebInfo (optimized with debug symbols)

**Profiling Tools:**
- Tracy Profiler for frame timing
- MSVC assembly inspection for vectorization verification
- RenderDoc for GPU profiling

**Test Scenarios:**
1. **Transform Update Test:** 100k entities, PrePhysics only, no physics
2. **Full Simulation Test:** 100k entities, PrePhysics + Update + PostPhysics
3. **Render Stress Test:** 1M entities, all visible, no culling
4. **Culling Test:** 100k entities, 30% visible (typical game scene)
5. **Network Rollback Test:** 100k entities, rollback 64 frames, resimulate

---

## Current Status vs Targets (Week 9)

| Target                    | Goal   | Current                          | Delta       | Status |
|---------------------------|--------|----------------------------------|-------------|--------|
| Full Frame (100k @ 512Hz) | 1.95ms | ~0.73ms steady, 18.74ms settling | ✅ Achieved! | ✅      |
| Physics (5.5k @ 64Hz)     | 0.8ms  | ~1ms steady, 14.67ms spike       | ✅ On target | ✅      |
| Render (100k @ 60 FPS)    | 8.5ms  | ~0.88ms (dirty upload, no cull)  | ✅ Excellent | ✅      |
| Render (205k @ 60 FPS)    | —      | ~1.5ms steady, ~3.1ms settling   | ✅ Bonus     | ✅      |
| Memory (100k, 128 pages)  | 685 MB | ~2.2GB w/ 10k Jolt bodies        | ⏳ Pending   | 🔄     |
| Input Latency             | <16ms  | ~9ms on 240Hz monitor            | ✅ Achieved! | ✅      |
| Rollback (5fr, 100k ent)  | <5ms   | ~18ms (full resim, no dirty opt) | ⏳ Pending   | 🔄     |
| Rollback Determinism      | exact  | Byte-perfect ECS + Jolt          | ✅ Achieved! | ✅      |

**Key Observations:**

- 100k cubes + 25-layer pyramid: render 1133 FPS steady, logic 1375 FPS steady, 9.24ms input-to-photon
- 205k entities (100k super cubes + 5.5k physics): render 320 FPS settling → 790 FPS steady
- Dirty-bit-driven selective GPU upload operational — only modified entities uploaded per frame
- Render FPS climbs as physics settles (fewer dirty entities → less upload work)
- FieldProxy SIMD optimization was the breakthrough (4x speedup)
- Memory usage is pretty massive with rollback enabled, around 270MB without
- JoltBody moved from cold to volatile tier — FlushPendingBodies now iterates contiguous DUAL+PHYS slab region directly
- 25-layer pyramid (5,526 entities) settling in ~14.67ms, steady state ~1ms — dense pyramid forms monolithic Jolt island
  limiting parallelism during settling
- Rollback resim: ~18ms for 5 frames with 100k entities — full frame resim without dirty propagation optimization.
  Jolt snapshot restore <1ms (7KB). Dirty propagation expected to bring this well under the <5ms target for typical
  corrections.

---
