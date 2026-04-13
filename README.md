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

Test meshes and rigs from https://www.3dfiggins.com/Store/

---

Sync and Build: [docs/BUILD_OPTIONS.md](docs/BUILD_OPTIONS.md)

---

## Current Status (2026-04)

**Performance:**

- **Sentinel (Main):** 1.0ms per frame, input polling with lock-free double-buffered input.
- **Brain (Logic):**
    - **15-layer Pyramid (1,240 cubes), 512Hz Logic / 64Hz Physics:**
        - avg frame time: 1ms (capped at 1024 FPS)
        - avg physics time: 58μs, 105μs on Jolt pull update frames
      - avg input-to-photon: 7.37ms
    - **25-layer Pyramid (5,525 cubes), 512Hz Logic / 64Hz Physics:**
        - avg frame time: 1ms, max 15.58ms under load
        - avg physics time: 1.16ms, 2.44ms on Jolt pull update frames
        - avg input-to-photon: 13.25ms, max 18.08ms under load
    - **100k cubes + 25-layer pyramid (105k entities):**
        - 0.73ms steady, 18.74ms settling. 1375 FPS steady.
    - **205k entities (100k super + 5.5k physics):**
        - ~1.4ms steady, 28ms settling. 512Hz maintained throughout.
- **Encoder (Render):** 0.73ms per frame (100k entities), ~1.5ms (205k entities)

**Architecture:**
- ✅ Three-thread architecture (Sentinel/Brain/Encoder)
- ✅ Raw Vulkan (volk 1.4.304 + VMA 3.3.0), migrated to vk::raii::
- ✅ SoA component decomposition (FieldProxy with Scalar/Wide/WideMask)
- ✅ EntityView hydration (zero virtual calls)
- ✅ SIMD-friendly batch processing (AVX2)
- ✅ Dirty-bit selective GPU upload (RenderAck handshake, 5 dirty bitplanes, AVX2 scan)
- ✅ GPU-driven compute pipeline (predicate → prefix_sum → scatter, Slang shaders)
- ✅ Temporal component N-frame buffer (TemporalComponentCache)
- ✅ Lock-free job system (MPMC ring buffers, futex-based wake, per-chunk dispatch)
- ✅ Tiered storage (dual-ended arena partition layout, 4 tiers: Cold/Static/Volatile/Temporal)
- ✅ Jolt Physics v5.5.0 (slab-direct iteration, awake-only pull, 512Hz/64Hz lockstep)
- ✅ Rollback substrate (ECS + Jolt byte-perfect snapshot ring buffer — delta compression and rollback netcode pending)
- ✅ Construct/View OOP layer (Construct<T>, Owned<T>, ConstructView<TEntity>, JoltCharacter)
- ✅ Editor (bare-bones): scene hierarchy, entity inspection, reflected properties, save/load, PIE
- ✅ Networking: GNS, entity spawn replication, state corrections, PIE loopback
- 🔧 Game flow: FlowManager, GameState, GameMode. Toolbox travel model (composable primitives, not single policy). In
  progress.

---

## Core Features

### Mental Model: Global Cache + `EntityCacheIndex`

TrinyxEngine stores gameplay data in SoA slabs. A simple way to visualize the cache is a spreadsheet:

- **Columns** are entities, indexed by `EntityCacheIndex`.
- **Rows** are fields (SoA arrays) generated from components (`Transform.PosX`, `Transform.PosY`, `Health.Value`, …).
- **Cell** `(field, EntityCacheIndex)` is the value for that entity in that field.

Volatile and Temporal are different row ranges in the same global model (different tiers / history buffers), not
separate
entity spaces. Chunk allocation claims contiguous column ranges based on `EntitiesPerChunk`, which is why
`EntityCacheIndex`
is globally valid across tiers and makes direct slab iteration fast.

Even if you only interact with the engine through `Construct<T>` and Views, understanding that your data ultimately
lives
in field rows indexed by `EntityCacheIndex` helps you reason about performance, determinism, and what “moving entities”
or “defrag” actually means.

### The Trinyx Trinity (Three-Thread Architecture)

- **Sentinel (Main Thread):** 1000Hz input polling, window + Vulkan lifetime management. Bit of a waste to do this all
  on its own, probably something we can do to give it some work to do inbetween, simple jobs queue?
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
- **Physics Queue** — Jolt jobs (workers produce and consume; 25% of workers dedicated by default)
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

| Tier     | Structure                | Frames            | Rollback | Use Case                                     |
|----------|--------------------------|-------------------|----------|----------------------------------------------|
| Cold     | Archetype chunks (AoS)   | 0                 | No       | Rarely-updated, non-iterable data            |
| Static   | Separate read-only array | 0                 | No       | Geometry, never changes                      |
| Volatile | SoA ring buffer          | 3 (triple-buffer) | No       | Cosmetic entities, ambient AI, particles     |
| Temporal | SoA ring buffer          | max(8, X)         | Yes      | Networked, simulation-authoritative entities |

Within each tier, entities are placed into one of two fixed-size **arenas** using a dual-ended allocator:

- **Arena 1 (Renderable)** `[0..MAX_RENDERABLE_ENTITIES)` — RENDER bucket grows right from 0, DUAL bucket grows left
  from MAX_RENDERABLE. DUAL and PHYS are contiguous at the boundary seam so the physics solver iterates them as one
  dense pass.
- **Arena 2 (Cached)** `[MAX_RENDERABLE_ENTITIES..MAX_CACHED_ENTITIES)` — PHYS bucket grows right, LOGIC bucket grows
  left.

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
// Component — SoA fields declared with TNX_TEMPORAL_FIELDS or TNX_VOLATILE_FIELDS
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct CVelocity : ComponentView<CVelocity, WIDTH>
{
    TNX_TEMPORAL_FIELDS(CVelocity, Physics, vX, vY, vZ)

    FloatProxy<WIDTH> vX, vY, vZ;
};
TNX_REGISTER_COMPONENT(CVelocity)

// Entity — composes components; TNX_REGISTER_SCHEMA wires SoA layout
template <FieldWidth WIDTH = FieldWidth::Scalar>
class EMyEntity : public EntityView<EMyEntity, WIDTH>
{
    TNX_REGISTER_SCHEMA(EMyEntity, EntityView, Transform, Vel)
public:
    CTransform<WIDTH> Transform;
    CVelocity<WIDTH>  Vel;

    void PrePhysics(SimFloat dt) {
        Transform.PosX += Vel.vX * dt;
        Transform.PosY += Vel.vY * dt;
        Transform.PosZ += Vel.vZ * dt;
    }
};
TNX_REGISTER_ENTITY(EMyEntity)
```

Users write natural OOP code while the engine handles SoA layout, double-buffering, and SIMD automatically.

`FieldProxy` has three width modes: `Scalar` (one entity/iteration), `Wide` (8 entities/AVX2 instruction),
and `WideMask` (tail-masked partial lanes for non-multiples of 8).

### Construct/View OOP Layer

Two object types coexist:

- **Constructs** — Singular complex OOP objects (`Construct<Player>`, `Construct<GameMode>`). Own Views into ECS data,
  hold bespoke logic, auto-register ticks via C++20 concepts. Compose via `Owned<T>` members.
- **Entities** — Raw ECS data for high-count homogeneous objects (zombies, bullets, particles). No bespoke logic. Engine
  sweeps them with wide SIMD.

```cpp
class Player : public Construct<Player>
{
    ConstructView<EPlayer> Body;            // Transform+mesh+color+scale in DUAL partition
    JoltCharacter CharacterController;      // Capsule character (no ECS JoltBody)
    Owned<CameraConstruct> FirstPersonCam;  // Eye-height camera
    Owned<CameraConstruct> ThirdPersonCam;  // Behind + above

    void InitializeViews();               // Create Views, set up character controller
    void PrePhysics(SimFloat dt);         // WASD → desired velocity
    void PhysicsStep(SimFloat dt);        // Drive JoltCharacter, write position to slab
    void ScalarUpdate(SimFloat dt);       // Mouse look, camera toggle, camera positioning
};
```

Tick methods are detected at compile time — implement the method, get the tick. Don't implement it, pay nothing.
ConstructBatch dispatches via type-erased function pointers (no virtual calls), sorted by TickGroup.

`ConstructView<TEntity>` is a generic template that works with any EntityView type. It creates a backing ECS entity,
hydrates FieldProxy cursors, and auto-rehydrates on frame advance and defrag. Partition is auto-derived from component
SystemGroup tags.

### Networking

Server-authoritative model with GNS (GameNetworkingSockets) transport:

- **PIENetThread** dispatches messages to `ServerNetThread` and `ClientNetThread` sub-handlers within the same process
- **ServerNetThread** injects per-player `InputFrame` packets into per-owner `InputBuffer` slots (
  `World::GetPlayerSimInput(ownerID)`), ensuring client input drives only that player's Construct
- **ReplicationSystem** walks the server Registry each net tick:
    - Sends `EntitySpawn` (reliable) for new entities — includes ClassID, transform, scale, color, mesh
    - Sends batched `StateCorrection` (unreliable) with authoritative transforms
- **Soul RPC system** — type-safe server/client RPC dispatch (`TNX_IMPL_SERVER` / `TNX_IMPL_CLIENT`) used for
  `PlayerBegin` handshake and `PlayerBeginConfirm`
- **PIE loopback** — editor creates server + N client Worlds in same process for local testing
- **EntityNetHandle** — packed uint32 (NetOwnerID:8 + NetIndex:24) for network entity identity

```bash
# Playground networking modes
Playground --server --port 27015
Playground --client 127.0.0.1 --port 27015
Playground                                    # standalone, local player
```

### Replay & Recording (Architectural Win)

The slab-based SoA layout makes replay recording nearly free — all deterministic simulation state already lives in
contiguous, trivially-copyable field arrays. Adding replay is just serialization in `PropagateFrame`.

**Compression advantage:** Homogeneous float arrays (all `PosX`, all `PosY`, etc.) have high spatial coherence. Delta
compression between frames yields mostly zeros. Far superior to AoS formats where mixed types destroy compression
ratios.

**Free wins from existing architecture:**

- **Kill cam / rewind** — slab snapshots in ring buffer; rewind = index backward + re-render with different camera
- **Spectator scrubbing** — random access seek via snapshot index, resume simulation forward
- **Anti-cheat validation** — diff server vs client slab timelines; divergence flags cheating
- **Bandwidth estimation** — compressed delta size = theoretical minimum sync payload for netcode
- **Sub-tick precision** — input events timestamped at actual ms read time, not frame-quantized
- **With rollback** — retroactive event insertion enables frame-perfect multiplayer reproduction from any perspective

---

## Architectural Constraints

### Hard Constraints
1. **No virtual functions** in entities or components — compile-time enforced by `TNX_REGISTER_ENTITY`
    1. This constraint does not extend to the actual OOP side of the engine, GameModes, Pawns, Player Controllers, AI
       and State Management, etc.
2. **PoD Components only** — All components intended to be used with the DoD system must be comprised of FieldProxy<T,
   WIDTH> structs.
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
- **[Game Flow](docs/FLOW.md)** — FlowManager, GameState stack, travel model
- **[GameMode & Soul](docs/GAMEFLOW.md)** — GameMode lifecycle, player join/spawn flow, Soul RPC system
- **[Networking](docs/NETWORKING.md)** — GNS transport, replication, input injection, PIE loopback
- **[Performance Targets](docs/PERFORMANCE_TARGETS.md)** — Benchmarks, budgets, scalability analysis
- **[Data Structures](docs/DATA_STRUCTURES.md)** — FieldProxy, EntityView, InstanceData, component patterns
- **[Configuration Guide](docs/CONFIGURATION.md)** — EngineConfig presets and tuning
- **[Current Status](docs/STATUS.md)** — Progress log, roadmap, next milestones
- **[Build Options](docs/BUILD_OPTIONS.md)** — CMake configuration, Tracy profiling, vectorization
- **[Defragmentation](docs/DEFRAGMENTATION.md)** — Entity slot defrag, chunk mirror compaction, View rehydration
  contract
- **[Determinism](docs/DETERMINISM.md)** — Deterministic simulation rules, EntityCacheIndex stability, networking
  contract
- **[Schema Error Examples](docs/SCHEMA_ERROR_EXAMPLES.md)** — Reflection system mistakes and fixes
- **[GPU-Driven Rendering Design](docs/GPU_Driven_Rendering_Design.md)** — GPU pipeline design doc

---

## Building

### Prerequisites

**System Requirements:**
- **CMake:** 3.20+
- **Compiler:** GCC 10+ / Clang 12+ (Linux), MSVC 2022+ (Windows)
- **C++ Standard:** C++20

**Git Submodules (required):**
```bash
# Clone with submodules
git clone --recursive https://github.com/YourRepo/TrinyxEngine.git

# Or if already cloned, initialize submodules
git submodule update --init --recursive
```

**Submodules include:**
- Jolt Physics v5.5.0 — Physics simulation
- Tracy v0.13.1 — Profiler
- Dear ImGui (docking branch) — Editor UI
- ImGuizmo — 3D gizmo manipulation
- GameNetworkingSockets — Networking layer
- OpenSSL 3.3.3 — Crypto for networking
- Protocol Buffers 3.29.2 — Serialization

**Note:** Submodule download is ~1.8GB. First build will take 10-20 minutes due to OpenSSL and Protobuf compilation.

### Quick Start

```bash
# Standard build
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
./build/Testbed/Testbed

# Editor build (recommended for development)
cmake -B build-editor -DTNX_ENABLE_EDITOR=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-editor
./build-editor/Testbed/Testbed

# Windows (Visual Studio)
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo
.\build\Testbed\RelWithDebInfo\Testbed.exe
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `TNX_ENABLE_EDITOR` | OFF | Enable ImGui editor with GPU picking |
| `TNX_ENABLE_ROLLBACK` | OFF | Enable N-frame rollback for netcode |
| `ENABLE_TRACY` | ON | Tracy profiler integration |
| `ENABLE_AVX2` | ON | AVX2 SIMD instructions |

**Example:**
```bash
# Editor build with profiling
cmake -B build -DTNX_ENABLE_EDITOR=ON -DENABLE_TRACY=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

See [docs/BUILD_OPTIONS.md](docs/BUILD_OPTIONS.md) for complete configuration reference.

---

## Performance Philosophy

**Target:** 100,000 dynamic entities at 512Hz fixed update (1.95ms per frame)

| Thread           | Target              | Current                                                  |
|------------------|---------------------|----------------------------------------------------------|
| Sentinel         | 1.0ms (1000 Hz)     | ✅ Achieved                                               |
| Brain (Logic)    | 1.95ms (512 Hz)     | 0.73ms steady (100k entities), ~1.4ms (205k)             |
| Encoder (Render) | 8-16ms (60-120 FPS) | 0.88ms (100k), 1.5ms (205k). Dirty-bit selective upload. |

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

---

## Contact

- **Author:** Cody "Tyko" Pederson

---

**Note:** This is a personal R&D project for experimenting with engine architecture. Production use is not recommended.
The primary goal is stress-testing architectural theories that would be too risky to implement in a live product.
