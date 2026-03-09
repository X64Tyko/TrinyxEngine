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

**Performance:**
- **Sentinel (Main):** 1.0ms per frame (1000 Hz) ✅
- **Brain (Logic):** ~1.7ms per frame (target: 1.95ms @ 512Hz)
- **Encoder (Render):** Orange cube rendering at full rate via BDA pipeline ✅
- **100k entities:** ~0.3ms PrePhysics, ~1.7ms full logic frame

**Architecture:**
- ✅ Three-thread architecture (Sentinel/Brain/Encoder)
- ✅ Raw Vulkan (volk 1.4.304 + VMA 3.3.0), migrated to vk::raii::
- ✅ SoA component decomposition (FieldProxy with Scalar/Wide/WideMask)
- ✅ EntityView hydration (zero virtual calls)
- ✅ SIMD-friendly batch processing (AVX2)
- ✅ Dirty bit delta tracking (TemporalFlagBits::Dirty in universal strip)
- ✅ GPU-driven compute pipeline (predicate → prefix_sum → scatter, Slang shaders)
- ✅ Temporal component dual-buffer (TemporalComponentCache, proto-slab)
- ✅ VulkRender Steps 1–3: clear → indexed cube pipeline → GpuFrameData + BDA draw
- ✅ Lock-free job system (MPMC ring buffers, futex-based wake, per-chunk dispatch)
- ✅ Core-aware thread pinning (physical cores first, SMT siblings second)
- ✅ GameManager CRTP pattern (TNX_IMPLEMENT_GAME macro, zero-boilerplate project setup)
- ✅ Project-relative INI config (*Defaults.ini scanning from source directory)
- ✅ VulkRender Step 4: entity data wired from TemporalComponentCache (functional, optimization pending)
- ✅ Tiered storage (dual-ended arena partition layout, 4 tiers: Cold/Static/Volatile/Temporal)
- ⏳ Physics integration (Jolt, not started)

---

## Core Features

### The Trinyx Trinity (Three-Thread Architecture)

- **Sentinel (Main Thread):** 1000Hz input polling, window + Vulkan lifetime management, frame pacing
- **Brain (Logic Thread):** 512Hz fixed-timestep coordinator. Dispatches per-chunk jobs, then steals work while waiting.
- **Encoder (Render Thread):** Variable-rate render coordinator. Dispatches GPU prep jobs, then steals work while
  waiting.
- **Worker Pool:** Core-aware pinned threads pulling from Physics/Render/General queues (work-stealing).

Brain and Encoder are coordinators, not dedicated workers. They dispatch jobs then call `WaitForCounter`,
which makes them steal work from all queues while waiting — zero idle time. On an 8-core CPU, the 5 remaining
cores form the worker pool, giving ~6× effective parallelism for logic and render passes.

### Job System

Lock-free MPMC job dispatch with three priority queues:

- **Physics Queue** — PrePhysics/PostPhysics per-chunk jobs (Brain produces, all consume)
- **Render Queue** — GPU upload/compute dispatch (Encoder produces, all consume)
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
  iterates this range exclusively.
- **Arena 2 (Cached)** `[MaxPhysicsEntities..MaxCachedEntities)` — RENDER bucket grows right, LOGIC bucket grows left.

Partition group (Dual/Phys/Render/Logic) is derived automatically from the `SystemGroup` tags on each component. No
manual annotation required.

See [Architecture Documentation](docs/ARCHITECTURE.md) for full details.

### GPU-Driven Rendering Pipeline

A 3-pass compute pipeline processes entity data on the GPU each frame:

1. **Predicate** — mark active + visible entities (writes `scan[i] = 0 or 1`)
2. **Prefix Sum** — Option-B scan (subgroup lanes + one `atomicAdd` per workgroup)
3. **Scatter** — lerp fields for GPU interpolation, write to InstanceBuffer SoA, set `DrawArgs.instanceCount`

The render thread uploads only **dirty entities** each frame (delta upload via cumulative dirty bit array).
GPU interpolation uses its own persistent previous-frame InstanceBuffer — render thread only uploads current
frame T; the GPU keeps T-1 itself. Five GPU InstanceBuffers decouple the VSync clock from the logic thread.

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
2. **PoD Components only** — no `std::vector` or `std::string` (enforced by `VALIDATE_COMPONENT_IS_POD`)
3. **Zero frame allocations** — no heap allocation in PrePhysics/PostPhysics/Render
4. **Lock-free inter-thread communication** — atomics and lock-free structures only
5. **GPU calls only on the Encoder thread**

### Design Goals
6. **White Box Philosophy** — understand and debug everything
7. **OOP Facade** — natural syntax despite SoA layout
8. **Cache Locality First** — sequential access patterns, partition-aligned entity groups
9. **SIMD-Friendly** — vectorizable loops (AVX2), AVX-512 ready
10. **Deterministic Option** — configurable for rollback netcode

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
- Tracy submodule (`libs/tracy`)
- Slang compiler binary (`libs/slang/bin/slangc`) — vendored

```bash
cmake -B cmake-build-relwithdebinfo -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build cmake-build-relwithdebinfo
./cmake-build-relwithdebinfo/Testbed/Testbed
```

See [docs/BUILD_OPTIONS.md](docs/BUILD_OPTIONS.md) for advanced configuration.

---

## Performance Philosophy

**Target:** 100,000 dynamic entities at 512Hz fixed update (1.95ms per frame)

| Thread           | Target              | Current                           |
|------------------|---------------------|-----------------------------------|
| Sentinel         | 1.0ms (1000 Hz)     | ✅ Achieved                        |
| Brain (Logic)    | 1.95ms (512 Hz)     | ~1.7ms                            |
| Encoder (Render) | 8-16ms (60-120 FPS) | Functional (optimization pending) |

The tiered partition design eliminates the cross-archetype co-indexing problem: physics iterates
DUAL→PHYS→STATIC as three dense SIMD passes; render iterates DUAL→RENDER→STATIC. No pointer chasing,
no per-chunk header lookups, no data duplication.

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

---

## Contact

- **Author:** Cody "Tyko" Pederson
- **Issues:** [GitHub Issues](https://github.com/YourUsername/TrinyxEngine/issues)

---

**Note:** This is a personal R&D project for experimenting with engine architecture. Production use is not recommended.
The primary goal is stress-testing architectural theories that would be too risky to implement in a live product.
