# Performance Targets

> [Home](../Home.md) | [Status & Roadmap →](Status-And-Roadmap.md)

---

## Core Budget: 512Hz = 1.95ms per Frame

| Phase | Budget | Target |
|---|---|---|
| PrePhysics | 0.4ms | User logic, input, AI decisions |
| Physics Simulation | 0.8ms | Jolt solver, collision, response |
| PostPhysics | 0.3ms | Collision callbacks, state updates |
| History Write | 0.2ms | Temporal ring buffer write |
| Overhead | 0.25ms | Scheduling, atomics, profiling |
| **TOTAL** | **1.95ms** | **512Hz target** |

### Measured (2026-05, RelWithDebInfo)

| Test | Result |
|---|---|
| PrePhysics (100K entities) | ~0.1ms |
| PrePhysics (1M entities, stress test) | ~1.0ms |
| Full logic frame (100K, no physics) | ~0.3ms with propagation |
| 25-layer pyramid (5,526 entities) | ~1ms steady, 14.67ms settling spike |
| 100K cubes + 25-layer pyramid (logic) | 0.73ms steady, 18.74ms settling |
| 205K entities (100K super + 5.5K phys) | ~1.4ms steady, ~28ms settling |
| Jolt step (25-layer settling) | ~12ms settling, <1ms steady |

---

## Render Thread

Target: 60–120 FPS (8–16ms per frame) for 100K visible entities.

| Phase | Budget | Status |
|---|---|---|
| History Access | 0.5ms | Read T-1 and T sections from slab |
| Culling + Interpolation | 3.0ms | Frustum cull, lerp, build InterpBuffer |
| State Sorting | 1.0ms | Sort by 64-bit keys (pending) |
| GPU Upload | 1.5ms | Dirty-bit selective transfer |
| Command Encoding | 2.0ms | Build render pass |
| GPU Submit + Sync | 0.5ms | Frame fence, swapchain |
| **TOTAL** | **8.5ms** | **~117 FPS budget** |

### Measured

| Test | Result |
|---|---|
| 100K cubes + 25-layer pyramid (render) | ~0.88ms (1133 FPS) |
| 205K entities (settling) | ~3.1ms (320 FPS) |
| 205K entities (steady) | ~1.5ms (660 FPS) |

No frustum culling yet; all entities rendered. Dirty-bit-driven GPU upload operational — only modified entities uploaded per frame.

---

## Input-to-Photon Latency

Target: <16ms (one frame @ 60Hz).

| Stage | Avg |
|---|---|
| Input Sampling (Sentinel) | 0.5ms |
| Logic Processing (Brain) | 1.95ms (one 512Hz tick) |
| Render Frame (Encoder) | 0.7ms |
| GPU Execution + VSync | 16.6ms @ 60Hz monitor |
| **Best case total** | **18.75ms** |

**Measured:** ~9ms on 240Hz monitor steady state. ~14.3ms under heavy physics load (205K entities settling).

---

## Rollback Performance

Target: <5ms for 128-frame resimulation.

| Metric | Result |
|---|---|
| Resim (5 frames, 100K entities) | ~18ms (full resim, no dirty propagation) |
| Resim (12 frames, 100K entities) | ~28ms (crosses 2 physics boundaries) |
| Jolt RestoreSnapshot | <1ms (7KB via `StateRecorderImpl`) |
| ECS determinism | Byte-perfect (34MB memcmp) |
| Jolt determinism | Byte-perfect (7,436 bytes) |
| Per-frame SaveSnapshot overhead | <0.1ms |

Full frame resim includes PrePhysics + Physics Step + FlushPendingBodies + PullActiveTransforms + PostPhysics + PropagateFrame for every frame in the window. **Dirty propagation** (resim only corrected entities) is designed but not yet wired — expected to reduce resim cost by 10–100× for typical corrections affecting <1% of entities.

---

## Scalability

### Entity Count vs Logic Frame Time

| Entity Count | PrePhysics (512Hz) | Notes |
|---|---|---|
| 10K | 0.01ms | Trivial |
| 50K | 0.08ms | Comfortable |
| 100K | ~0.1ms | Primary target — achieved |
| 1M | ~1.0ms | Stress test only |

### Entity Count vs Render Frame Time (no culling)

| Count | Render | FPS |
|---|---|---|
| 100K | ~0.88ms | 1133 |
| 205K (settling) | ~3.1ms | 320 |
| 205K (steady) | ~1.5ms | 660 |

---

## Memory Targets

### Per-Entity Hot Data (Approximate)

| Component | Fields | Bytes/entity/frame |
|---|---|---|
| CTransform | 9 fields (pos/rot/scale) | 36B |
| CVelocity | 3 fields | 24B |
| CJoltBody | 4 fields | ~16B |

### Temporal Slab (100K entities)

Temporal slab size = `MAX_CACHED_ENTITIES × bytes per entity × TemporalFrameCount`. At 92 bytes/entity
([ECS & Storage](../architecture/ECS-And-Storage.md#memory-sizing)):

| Ring depth | History @ 512Hz | Memory |
|---|---|---|
| 8 (minimum) | ~15.6ms | ~74MB |
| 32 (engine default) | ~62.5ms | ~294MB |
| 128 (editor / full rollback) | ~250ms | ~1.18GB |

The 34MB figure in [Rollback Netcode](../networking/Rollback-Netcode.md) is the temporal field data compared byte-for-byte
in the 2026-03-29 validation run, not the full ring allocation. Measured allocations scale with the configured
`MAX_CACHED_ENTITIES` and component set — see `EngineConfig::TemporalFrameCount`.

---

## Network

Target: 30Hz net tick, <5ms resim per correction.

| Metric | Target |
|---|---|
| Packet send rate | 30Hz (configurable via `NetworkUpdateHz`) |
| Rollback window | 128 frames (~250ms @ 512Hz) |
| Resim time | <5ms (pending dirty propagation optimization) |
| Delta compression ratio | 70–90% |
| Prediction accuracy | >95% |

---

## Benchmark Hardware

Tests run on:
- Intel Core Ultra 9 275HX / AMD Ryzen 9 9950X
- 32GB / 64GB RAM
- RTX 5070 Ti / RTX 4070
- CachyOS Arch Linux, Windows 11
- Build: RelWithDebInfo

Profiling tools: Tracy (frame timing), MSVC assembly inspection, RenderDoc.
