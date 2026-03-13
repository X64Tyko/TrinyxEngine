# TrinyxEngine

**A high-performance, data-oriented game engine for R&D and experimentation**

---

## Executive Summary

**Purpose:** A personal R&D sandbox designed to strip away modern engine abstractions and validate that a strict
data-oriented architecture can deliver sub-millisecond latency—**without giving up the comfort and mental model of OOP.**

**Objective:** A high-performance, data-oriented engine prioritizing mechanical elegance and input-to-photon latency,
while maintaining as close to existing OOP style and structure on the user end as possible.

**Primary Target:** 100,000+ dynamic entities at 512Hz fixed update (1.95ms per frame budget)

**Philosophy:** White-box architecture - users can understand, debug, and modify the engine without black-box
abstractions. "Build it to break it." This project stress-tests architectural theories (tiered temporal storage,
GPU-driven rendering, lock-free communication) that are too risky to implement directly into a live commercial product.

---

## Current Status (2026-03)

**Performance (512Hz Logic / 64Hz Physics — 8× lockstep):**

- **Sentinel (Main):** 1.0ms per frame — lock-free double-buffered input at 1000Hz ✅
- **Brain (Logic):**
    - **15-layer pyramid (1,240 cubes):** avg 1ms frame, 58μs physics, 105μs on Jolt pull frames — 7.37ms input→photon
    - **25-layer pyramid (5,525 cubes):** avg 1ms (max 15.58ms under load), 1.16ms physics — 13.25ms input→photon (max 18.08ms)
    - **100k entities (no physics):** ~0.1ms PrePhysics, ~0.3ms full frame with propagation
- **Encoder (Render):** 0.73ms per frame (100k entities, no culling yet) ✅

**Architecture:**
- ✅ Three-thread architecture (Sentinel/Brain/Encoder)
- ✅ Raw Vulkan (volk 1.4.304 + VMA 3.3.0), migrated to vk::raii::
- ✅ SoA component decomposition (FieldProxy with Scalar/Wide/WideMask)
- ✅ EntityView hydration (zero virtual calls)
- ✅ SIMD-friendly batch processing (AVX2)
- ✅ Dirty bit delta tracking
- ✅ GPU-driven compute pipeline (predicate → prefix_sum → scatter, Slang shaders)
- ✅ Temporal component N-frame buffer (TemporalComponentCache, proto-slab)
- ✅ VulkRender Steps 1–4: clear → indexed cube pipeline → GpuFrameData + BDA draw → entity data wired from
  TemporalComponentCache
- ✅ Lock-free job system (MPMC ring buffers, futex-based wake, per-chunk dispatch)
- ✅ Core-aware thread pinning (physical cores first, SMT siblings second)
- ✅ GameManager CRTP pattern (TNX_IMPLEMENT_GAME macro, minimal boilerplate project setup)
- ✅ Project-relative INI config (*Defaults.ini scanning from source directory)
- ✅ Tiered storage (dual-ended arena partition layout, 4 tiers: Cold/Static/Volatile/Temporal)
- ✅ Rough Physics integration (Jolt Physics. pull physics data using AVX2 transpose and scatter, push to Jolt... it's
  coming)
- ✅ Input buffering (double buffered input, lock-free polling, store events and bitstate for querying)

---

## Core Features

### The Trinyx Trinity (Three-Thread Architecture)

- **Sentinel (Main Thread):** 1000Hz input polling, window + Vulkan lifetime management. Frame pacing via busy-wait tail.
- **Brain (Logic Thread):** 512Hz fixed-timestep coordinator. Dispatches per-chunk jobs, then steals work while waiting.
- **Encoder (Render Thread):** Variable-rate render coordinator. Dispatches write jobs for new frame data to GPU
  buffers, steals render jobs while waiting. Pushes new command buffer with current data ptrs when swap is ready.
- **Worker Pool:** Core-aware pinned threads pulling from Physics/Render/General queues (work-stealing). defaulted to
  25% workers dedicated to physics work.

Brain and Encoder are coordinators, not dedicated workers. They dispatch jobs then call `WaitForCounter`,
which makes them steal work from their respective queues while waiting — zero idle time. On an 8-core CPU, the 5
remaining
cores form the worker pool, giving ~6× effective parallelism for logic and render passes.

### Job System

Lock-free MPMC job dispatch with four priority queues:

- **Logic Queue** — PrePhysics/PostPhysics per-chunk jobs (Brain produces, all consume)
- **Render Queue** — GPU upload/compute dispatch (Encoder produces, all consume)
- **Physics Queue** — Jolt solver jobs (Workers produce, workers consume)
- **General Queue** — Everything else + overflow from full queues

Workers block via `std::atomic::wait()` (futex on Linux, WaitOnAddress on Windows) when idle —
zero CPU usage between bursts, ~1-2us wake latency. Jobs are 64-byte cache-line-aligned structs
with 48-byte lambda payloads, dispatched through a Vyukov bounded MPMC ring buffer.

### Project Setup

User projects inherit from `GameManager<Derived>` (CRTP) and use `TNX_IMPLEMENT_GAME` to wire up `main()`:

```cpp
class MyGame : public GameManager<MyGame> {
    const char* GetWindowTitle() const { return "My Game"; }
    bool PostInitialize(TrinyxEngine& engine) { /* spawn entities */ return true; }
};
TNX_IMPLEMENT_GAME(MyGame)
```

Engine configuration lives in `*Defaults.ini` files in the project source directory (not the build directory),
loaded automatically via `TNX_PROJECT_DIR` set by CMake.

### Memory Model: Tiered Storage

Entity data lives in one of four storage tiers based on access pattern and rollback requirements:

| Tier | Structure | Frames | Rollback | Use Case |
|------|-----------|--------|----------|----------|
| Cold | Archetype chunks (AoS) | 0 | No | Rarely-updated, non-iterable data |
| Static | Separate read-only array | 0 | No | Geometry, never changes |
| Volatile | SoA ring buffer | 5 | No | Cosmetic entities, ambient AI, particles |
| Temporal | SoA ring buffer | max(8, X) | Yes | Networked, simulation-authoritative entities |

Within each tier, entities are placed into one of two fixed-size **arenas** using a dual-ended allocator:

- **Arena 1 (Physics)** `[0..MaxPhysicsEntities)` — PHYS bucket grows right, DUAL bucket grows left. The physics solver
  iterates this range exclusively--or will soon...
- **Arena 2 (Cached)** `[MaxPhysicsEntities..MaxCachedEntities)` — RENDER bucket grows right, LOGIC bucket grows left.

Partition group (Dual/Phys/Render/Logic) is derived automatically from the `SystemGroup` tags on each component. No
manual annotation required.

See [Architecture Documentation](docs/ARCHITECTURE.md) for full details.

### GPU-Driven Rendering Pipeline

A 3-pass compute pipeline processes entity data on the GPU each frame:

1. **Predicate** — reads flags from field slab (`CurrFieldAddrs[0]`, bit 31 = active), writes `scan[i] = 0 or 1`
2. **Prefix Sum** — Option-B scan (subgroup lanes + one `atomicAdd` per workgroup)
3. **Scatter** — lerp fields between current and previous slab for GPU interpolation, write to InstanceBuffer SoA, set
   `DrawArgs.instanceCount`

The render thread copies SoA field arrays from the temporal/volatile caches into one of 5 PersistentMapped
field slabs when a new logic frame is detected. The GPU reads the current and previous slabs via BDA for
interpolation. 3 field slabs cycle independently of the 2 GPU frame-in-flight slots, decoupling VSync
from the logic thread. Dirty-bit-driven partial upload is tracked but not yet wired (currently full-slab copy).

### Data-Oriented Components

Components decompose into Structure-of-Arrays via `FieldProxy<T, FieldWidth>`:

```cpp
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct Transform {
    FieldProxy<float, WIDTH> PositionX, PositionY, PositionZ;
    FieldProxy<float, WIDTH> RotationX, RotationY, RotationZ;
    FieldProxy<float, WIDTH> ScaleX,    ScaleY,    ScaleZ;

    TNX_TEMPORAL_FIELDS(Transform, SystemGroup::None,
        PositionX, PositionY, PositionZ,
        RotationX, RotationY, RotationZ,
        ScaleX, ScaleY, ScaleZ)
};
TNX_REGISTER_COMPONENT(Transform)

template <FieldWidth WIDTH = FieldWidth::Scalar>
struct CubeEntity : EntityView<CubeEntity, WIDTH> {
    Transform<WIDTH> transform;
    Velocity<WIDTH>  velocity;

    FORCE_INLINE void PrePhysics(double dt) {
        transform.PositionX += velocity.VelocityX * static_cast<float>(dt);
    }

    TNX_REGISTER_SCHEMA(CubeEntity, EntityView, transform, velocity)
};
TNX_REGISTER_ENTITY(CubeEntity)
```

Users write natural OOP code while the engine handles SoA layout, double-buffering, and SIMD automatically.

`FieldProxy` has three width modes: `Scalar` (one entity/iteration), `Wide` (8 entities/AVX2 instruction),
and `WideMask` (tail-masked partial lanes for non-multiples of 8).

---

## Architectural Constraints

### Hard Constraints
1. **No virtual functions** in entities or components — compile-time enforced by `TNX_REGISTER_ENTITY`
    1. This constraint does not extend to the actual OOP side of the engine, GameModes, Pawns, Player Controllers, AI
       and State Management, etc.
2. **FieldProxy-backed components only** — All components used with the SoA/DoD system must expose their fields as
   `FieldProxy<T, WIDTH>` members; cold components use plain POD structs with `TNX_REGISTER_FIELDS`.
3. **Zero frame allocations** — no heap allocation in PrePhysics/PostPhysics/Render
4. **Lock-free inter-thread communication** — atomics and lock-free structures only
5. **GPU calls only on the Encoder thread** — maybe NVidia Reflex at some point, unnecessary ATM.

### Design Goals
6. **White Box Philosophy** — understand and debug everything
7. **OOP Facade** — natural syntax despite SoA layout
8. **Cache Locality First** — sequential access patterns, partition-aligned entity groups
9. **SIMD-Friendly** — vectorizable loops (AVX2), AVX-512 ready
10. **Deterministic Option** — configurable for rollback netcode
11. **OOP - ECS Hybrid** — seamless integration of OOP and ECS paradigms, with ECS used for state storage and quick
    manipulation, OOP for complex logic and classes with low instantiation.

---

## Documentation

- **[Architecture Overview](docs/ARCHITECTURE.md)** — Tiered storage, partition design, threading model, GPU upload
- **[Performance Targets](docs/PERFORMANCE_TARGETS.md)** — Benchmarks, budgets, scalability analysis
- **[Data Structures](docs/DATA_STRUCTURES.md)** — FieldProxy, EntityView, InstanceData, component patterns
- **[Configuration Guide](docs/CONFIGURATION.md)** — EngineConfig presets and tuning
- **[Current Status](docs/STATUS.md)** — Progress log, roadmap, next milestones
- **[Build Options](docs/BUILD_OPTIONS.md)** — CMake configuration, Tracy profiling, vectorization
- **[Schema Error Examples](docs/SCHEMA_ERROR_EXAMPLES.md)** — Reflection system mistakes and fixes
- **[GPU-Driven Rendering Design](docs/GPU_Driven_Rendering_Design.md)** — GPU pipeline design doc

---

## Building

**Requirements (Linux):**
- C++20 compiler (GCC or Clang)
- CMake 3.20+
- SDL3 system package (`sudo apt install libsdl3-dev` or distro equivalent)
- Slang compiler binary (`libs/slang/bin/slangc`) — vendored
- Git submodules: Tracy (`libs/tracy`) and Jolt Physics (`libs/jolt`) — run `git submodule update --init --recursive`
  after cloning

```bash
cmake -B cmake-build-relwithdebinfo -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build cmake-build-relwithdebinfo
./cmake-build-relwithdebinfo/Testbed/Testbed
```

See [docs/BUILD_OPTIONS.md](docs/BUILD_OPTIONS.md) for advanced configuration.

---

## Performance Philosophy

**Target:** 100,000 dynamic entities at 512Hz fixed update (1.95ms per frame)

| Thread           | Target              | Current                                    |
|------------------|---------------------|--------------------------------------------|
| Sentinel         | 1.0ms (1000 Hz)     | ✅ Achieved                                 |
| Brain (Logic)    | 1.95ms (512 Hz)     | ~0.3ms with frame Propagation              |
| Encoder (Render) | 8-16ms (60-120 FPS) | 0.73ms. still needs dirty bit optimization |

The tiered partition design eliminates the cross-archetype co-indexing problem: physics iterates
DUAL→PHYS→STATIC as three dense SIMD passes; render iterates DUAL→RENDER→STATIC. No pointer chasing,
no per-chunk header lookups, no data duplication, some gaps, so far seems a worthwhile tradeoff.

See [docs/PERFORMANCE_TARGETS.md](docs/PERFORMANCE_TARGETS.md) for detailed analysis.

---

## Design Inspiration

- **Data-Oriented Design** — Naughty Dog
- **Overwatch Netcode** — GDC talks on lag compensation
- **GGPO** — Tony Cannon's rollback netcode
- **Jolt Physics** — zero-copy simulation integration
- **Tracy Profiler** — frame-accurate performance analysis

---

## License

MIT License — see [LICENSE](LICENSE) for details.

---

## Contact

- **Author:** Cody "Tyko" Pederson

---

**Note:** This is a personal R&D project for experimenting with engine architecture. Production use is not recommended.
The primary goal is stress-testing architectural theories that would be too risky to implement in a live product.
