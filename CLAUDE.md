# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

---

## Project Overview

**TrinyxEngine** is a C++20, data-oriented game engine R&D project built for competitive multiplayer games where determinism, rollback netcode, and input latency are first-class design constraints — not afterthoughts. The goal is to run 100,000+ dynamic entities at 512Hz fixed update (1.95ms/frame budget) while exposing a familiar OOP-style API (PlayerController, GameMode, GameState, behavior trees) to entity authors. It is structured as two CMake targets: `TrinyxEngine` (static library) and `Testbed` (executable).

This is not a general-purpose engine competing on breadth. It is the correct architecture for a specific and important
problem: competitive multiplayer simulations where two clients running the same inputs must produce bit-identical state,
and where input latency is a design constraint at the substrate level. It is aimed at being developer friendsly,
offering benefits like optional determinism, rollback, and low latency by default without sacrifices.

---

## Build Commands

**Requirements:**
- CMake 3.20+
- C++20 compiler (GCC 10+, Clang 12+, MSVC 2022+)
- Git submodules: Jolt Physics, Tracy, Dear ImGui (docking branch), ImGuizmo, GameNetworkingSockets, OpenSSL, Protobuf
- Vendored libraries: SDL3, Volk, VMA, Slang (already included in repo)

**First-time setup:**
```bash
# Clone with submodules
git clone --recursive https://github.com/YourRepo/TrinyxEngine.git

# Or if already cloned, initialize submodules
git submodule update --init --recursive
```

**Build:**
```bash
# Standard development build (RelWithDebInfo recommended for profiling)
cmake -B cmake-build-relwithdebinfo -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build cmake-build-relwithdebinfo

# Run the testbed
./cmake-build-relwithdebinfo/Testbed/Testbed

# Debug build
cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug

# Windows (Visual Studio)
cmake -B cmake-build-relwithdebinfo-visual-studio -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build cmake-build-relwithdebinfo-visual-studio --config RelWithDebInfo
```

**Key CMake options** (append to the configure step):

| Option | Default | Purpose |
|--------|---------|---------|
| `TNX_ENABLE_EDITOR=ON/OFF` | OFF | Enable editor UI (ImGui + GPU picking) |
| `TNX_ENABLE_ROLLBACK=ON/OFF` | OFF | Enable N-frame rollback history for netcode |
| `ENABLE_TRACY=ON/OFF` | ON | Tracy profiler integration |
| `TRACY_PROFILE_LEVEL=1/2/3` | 3 | 1=coarse (~1%), 2=medium (~5%), 3=per-entity (~50%+ overhead) |
| `ENABLE_AVX2=ON/OFF` | ON | `-march=native` on GCC/Clang |
| `GENERATE_ASSEMBLY=ON/OFF` | OFF | Emit `.s` files for vectorization inspection |
| `VECTORIZATION_REPORTS=ON/OFF` | OFF | Compiler loop-vectorization diagnostics |
| `TNX_ALIGN_64=ON/OFF` | OFF | 64-byte vs 32-byte field array alignment |
| `TNX_DETAILED_METRICS=ON/OFF` | OFF | Per-frame latency breakdown logging |

**Example builds:**
```bash
# Editor build (automatically enables GPU picking)
cmake -B build-editor -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTNX_ENABLE_EDITOR=ON
cmake --build build-editor

# Networked build with rollback
cmake -B build-netcode -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTNX_ENABLE_ROLLBACK=ON
cmake --build build-netcode
```

---

## Architecture Philosophy

Three interlocking design decisions define everything else:

1. **Fixed 512Hz logic update.** This is a constraint, not a limitation — If we can't improve the internet itself, we
   improve the software.

2. **Tiered SoA storage with rollback as a first-class citizen.** Hot component data lives in SoA ring buffers (Temporal: N-frame rollback history, Volatile: triple-buffer no rollback). Cold data lives in archetype chunks. The tier is determined by the component macro, not by the entity. An entity's effective tier is the highest tier of any of its components.

3. **OOP API over a data-oriented substrate.** The gameplay layer splits into two object types:
    - **Constructs** — Singular complex OOP objects (`Construct<Player>`, `Construct<GameMode>`). Own Views into ECS
      data, hold bespoke logic, auto-register ticks via C++20 concepts. Compose via `Owned<T>` members.
    - **Entities** — Raw ECS data for high-count homogeneous objects (zombies, bullets, particles). No bespoke logic.
      Engine sweeps them with wide SIMD.

   Developers write familiar OOP patterns on Constructs; the engine decomposes Views into SoA field arrays
   transparently. Developers need minimal understanding of the substrate to use it — DoD and ECS concepts, not a working
   knowledge; they only need to understand it if they're extending it.

---

## Threading Model — The Trinyx Trinity

Three dedicated threads + a shared worker pool:

- **Sentinel (main thread):** 1000Hz input polling, Engine static data management.
- **Brain (logic thread):** 512Hz fixed-timestep coordinator. Submits `LogicQueue` jobs, then acts as a worker until jobs complete.
- **Encoder (render thread):** Variable-rate render coordinator. Submits `RenderQueue` jobs, then acts as a worker.
- **Worker pool:** The remaining cores pull from both queues (work-stealing).

Brain and Encoder are **coordinators, not dedicated workers**. On an 8-core CPU this gives ~4× speedup for both logic and render passes.

### Four Job Queues

- **Logic Queue** — PrePhysics/PostPhysics per-chunk jobs (Brain produces, all consume)
- **Render Queue** — GPU upload/compute dispatch (Encoder produces, all consume)
- **Physics Queue** — Jolt physics solver jobs (Workers produce and consume; 25% of workers dedicated by default)
- **General Queue** — Everything else + overflow

---

## Memory Model: Tiered Storage

### Four Storage Tiers

| Tier     | Structure                | Frames    | Rollback | Use Case                                     |
|----------|--------------------------|-----------|----------|----------------------------------------------|
| Cold     | Archetype chunks (AoS)   | 0         | No       | Rarely-updated, non-iterable data            |
| Static   | Separate read-only array | 0         | No       | Geometry/mesh data, never changes            |
| Volatile | SoA ring buffer          | 3         | No       | Cosmetic entities, ambient AI, particles     |
| Temporal | SoA ring buffer          | max(8, X) | Yes      | Networked, simulation-authoritative entities |

**Important:** If `TNX_ENABLE_ROLLBACK` is disabled, the Temporal tier is not instantiated and Temporal components are treated as Volatile. Games that don't need rollback pay no memory cost for it.

### Partition Layout: Dual-Ended Arenas

```
Arena 1: Physics  [0 .. MAX_PHYSICS_ENTITIES)
  PHYS  (→) starts at 0           — physics-only (triggers, invisible movers)
  DUAL  (←) starts at MAX_PHYSICS — physics + render (players, AI, physics props)

Arena 2: Cached  [MAX_PHYSICS_ENTITIES .. MAX_CACHED_ENTITIES)
  RENDER (→) starts at MAX_PHYSICS — render-only (particles, decals, ambient props)
  LOGIC  (←) starts at MAX_CACHED  — logic/rollback-only entities
```

Physics iterates Arena 1 only — a dense wall of 100% relevant entities. Render iterates the DUAL tail + RENDER head, which are contiguous at the arena boundary (effectively one scan). No pointer chasing. No irrelevant data in either pass.

### Entity Group Auto-Derivation

The partition group (Dual/Phys/Render/Logic) is computed automatically at `TNX_REGISTER_ENTITY` time from `SystemGroup` tags on components. There is no manual group annotation — it would be a footgun that shoots gaps into partition regions.

---

## Component System

### FieldProxy

All SoA fields are wrapped in `FieldProxy<T, WIDTH>`. There are three widths:

- `Scalar` — single-entity scalar update path
- `Wide` — AVX2 8-wide SIMD update (unconditional store)
- `WideMask` — AVX2 8-wide SIMD update with Active-flag masked store

`FieldProxyMask` is a zero-size base type used when a component has no stored fields but needs the masked-store behavior (e.g., flags-only components).

### Component Registration Macros

```cpp
TNX_TEMPORAL_FIELDS(ComponentName, SystemGroup, Field1, Field2, ...)  // Temporal SoA tier
TNX_VOLATILE_FIELDS(ComponentName, SystemGroup, Field1, Field2, ...)  // Volatile SoA tier
TNX_REGISTER_FIELDS(ComponentName, Field1, Field2, ...)               // Cold (chunk only)
```

### Schema Validation

`SchemaValidation.h` enforces two hard constraints at compile time:

1. No virtual functions (vtable breaks SoA decomposition)
2. All `DefineFields()`-registered fields must be `FieldProxy` types

A raw `float` added to a component but not registered in `DefineFields()` is inert — invisible to the SoA system, can't corrupt layout. The schema is the enforcement boundary. `VALIDATE_COMPONENT_IS_POD` is defense-in-depth; the load-bearing check is the schema's `Bind`/`Advance` requirement.

### Bitplanes and Gap Skipping

The active strip is scanned 64 entities at a time (one 64-bit word). A zero word (64 inactive entities) is skipped with no field data touched. This covers the gap between PHYS and DUAL buckets in Arena 1 at negligible overhead. For mixed words, `FieldProxy` uses AVX2 masked loads/stores (8-wide, branchless).

---

## Gameplay Object Model — Constructs & Entities

### Two Object Types

**Constructs** are singular, complex scalar objects — the things that think. A Player, a GameMode, a TurretBase, an
AIDirector. They inherit from `Construct<T>` (CRTP) and compose Views to access ECS data. Created via
`Registry::CreateConstruct<T>()`.

**Entities** are raw ECS data for the horde — zombies, bullets, debris, particles. No bespoke logic. The engine sweeps
them with wide SIMD. Registered via `TNX_REGISTER_ENTITY`.

### Construct<T>

CRTP base that owns the full lifecycle. Auto-registers ticks via `if constexpr` concept detection — implement the
method, get the tick. Don't implement it, pay nothing.

```cpp
class Player : public Construct<Player>, public InstanceView<Player> { ... };
class Decal  : public Construct<Decal>,  public RenderView<Decal> { ... };
```

### Composition via Owned<T>

Complex Constructs compose via `Owned<T>` value members — compile-time ownership, deterministic init/destroy order,
automatic lifetime management:

```cpp
class Turret : public Construct<Turret>, public InstanceView<Turret>
{
    Owned<BarrelAssembly>  Barrel;    // has its own InstanceView, own tick
    Owned<TargetingSystem> Targeting; // pure logic, LogicView only
    Owned<AmmoFeed>        Ammo;     // StatsView, no physics
};
```

Each owned Construct is exactly as heavy as it needs to be. `TargetingSystem` doesn't pay for a physics body. `AmmoFeed`
doesn't tick unless it implements a tick method. C++20 concepts on `Owned<T>` enable compile-time interface contracts (
`Targetable`, `Damageable`) — zero-cost replacement for runtime gameplay tag queries.

### Views

Views are CRTP lenses into ECS data. They hydrate FieldProxy cursors on initialization and register as defrag listeners
so cursors stay valid if the allocator moves data.

| View         | Components                          | Partition |
|--------------|-------------------------------------|-----------|
| InstanceView | Transform + PhysBody + SkeletalMesh | DUAL      |
| PhysView     | Transform + PhysBody                | PHYS      |
| RenderView   | Transform + SkeletalMesh            | RENDER    |
| LogicView    | Transform only                      | LOGIC     |

**Ownership chain:** Construct → Views → ECS Entities → Components → FieldArrays → FieldProxies

### Tick Dispatch — ConstructBatch

Constructs register into typed scalar batches on the Brain thread. Dispatch is type-erased and non-virtual:

```cpp
struct ConstructTickEntry { void* Object; void (*Fn)(void*); TickGroup Group; int16_t OrderWithinGroup; };
```

TickGroup is a fixed engine enum controlling execution order:

```cpp
enum class TickGroup : uint8_t { PreInput=0, Default=1, PostDefault=2, Camera=3, Late=4 };
```

Brain thread tick sequence:

```
PrePhysics wide sweep → ScalarPrePhysics batch (Constructs)
  → Physics step →
PostPhysics wide sweep → ScalarPostPhysics batch (Constructs)
  → Entity ScalarUpdate (targeted writes from Constructs into entity fields)
  → Construct ScalarUpdate batch (high-level OOP logic)
  → Render publish
```

The Entity ScalarUpdate slot is the bridge point: an AIDirector Construct thinks once, then writes target positions into
zombie entity fields. The zombie has no bespoke logic — it's swept by the wide PrePhysics pass.

### Serialization

Constructs do NOT serialize their own C++ members. Only View-owned ECS data is serialized. If a value is
designer-authored (e.g., MaxAmmo), it belongs in a component so it serializes through the existing ECS path. No
special-case codepath.

### Thread Safety

Tick registration follows the spawn handshake contract: the handshake window is the one safe place to mutate engine
state. Registration outside the window is deferred to the next handshake. One pattern for all state mutation.

---

## Entity Lifecycle

### Spawn

The calling thread provides a lambda and performs a synchronized handshake with the Logic thread at the top of its frame. The Logic thread allows the spawning thread to write new entity data, then the spawning thread signals Logic to continue. This is synchronous, thread-safe, and wraps the fundamental contract behind deferred, queued, and batched spawning variants.

### Despawn

Entity is requested for deletion → goes into a deletion queue → tombstoned (Active flag cleared) immediately. The tombstone propagates through all systems for free: GPU predicate pass stops drawing it, physics awake-only pull ignores it, 64-entity bitplane scan skips it. A deferred destroy then runs on the Logic thread to reclaim memory. Visual and physics despawn happen at tombstone time; memory reclamation happens at deferred destroy time. These are decoupled but aligned.

**Archetype slot management:** `RemoveEntity` tombstones in place — clears Active flag, moves slot to
`InactiveEntitySlots` for future reuse by `PushEntities`. Two counters track slots independently:
`AllocatedEntityCount` (high-water mark, never decremented — used by `GetAllocatedChunkCount` for iteration bounds) and
`TotalEntityCount` (live count, decremented on removal — used by `GetLiveChunkCount` for UI/diagnostics). Update loops
iterate all allocated slots; the bitplane/masked-store path skips tombstoned entities at zero cost.

**Handle recycling:** `FreeGlobalHandle` reclaims the record index immediately (generation-bumped). Local and net handle
indices enter `PendingLocalRecycles`/`PendingNetRecycles` and only move to the free pool when `ConfirmLocalRecycles()`/
`ConfirmNetRecycles()` is called after the safety window. This prevents ABA aliasing where OOP code or a remote client
holds a stale handle.

Spawn and despawn are serialized through the Logic thread by construction — the handshake model for spawn and deferred destroy on the Logic thread for despawn guarantee this.

---

## Physics: Jolt Integration

Jolt Physics v5.5.0 runs at a configurable fraction of the logic rate. Default: 512Hz logic / 64Hz physics (8:1 ratio). The ratio is configurable — a game needing high physics accuracy can set it 1:1.

**Key design decisions:**

- **Jolt owns physics state.** The ECS does not push state to Jolt every frame. Jolt's integrator advances bodies internally. ECS only writes to Jolt on explicit overrides (spawn, teleport, impulse, kinematic target).
- **Awake-only pull.** After each Jolt step, only awake bodies are pulled back into SoA WriteArrays. A scene with 50K bodies where 200 are moving pays for 200 pulls.
- **Only transforms are pulled.** Velocities stay in Jolt. Gameplay logic that needs velocity queries Jolt directly during ScalarUpdate.

### Rollback and Jolt

Jolt's `SaveState`/`RestoreState` is used for rollback. When rolling back to frame N, the engine snaps to the nearest Jolt execution frame at or before N (e.g., rollback to frame 100 → restore from frame 96 at 8:1 ratio), reloads Jolt state, and resimulates from there. This is intentional: at most 7 frames of physics approximation in the worst case, which is acceptable for competitive multiplayer.

**Known determinism wrinkle:** Jolt uses float32 internally. The fixed-point → float32 → fixed-point bridge at the physics boundary introduces potential non-determinism. At cell-relative scale (≤±500m), float32 precision is ≈0.05mm — finer than the 0.1mm unit definition, so the bridge is lossless in practice. This needs empirical validation before rollback netcode is considered correct.

---

## Fixed-Point Coordinate System

Designed for bit-identical simulation across platforms. `Fixed32` wraps `int32_t` with overloaded arithmetic.

```
1 unit = 0.1mm (100 micrometers)
1 meter   =     10,000 units
1 km      = 10,000,000 units
Cell size (1km): 5,000,000 units → 430× headroom before overflow
World size (int64 cell origins): ~45M km at 0.1mm precision
```

`FieldProxy<Fixed32, WIDTH>` is used for all position, velocity, and force fields. A build variable can swap `FieldProxy<float>` for `FieldProxy<Fixed32>` without code changes in entity authors.

The only lossy step is render thread upload: fixed-point → camera-relative float32 for the GPU. Precision loss is ≈0.05mm at 1km — imperceptible and acceptable.

**Status:** Designed and documented. Not yet fully wired. Fixed-point coordinate system is on the cleanup-pass TODO list before netcode implementation.

---

## GPU Upload Architecture

Three-pass GPU-driven compute pipeline (Slang shaders):

1. **predicate.slang** — reads Active bit (bit 31 of flags), writes `scan[i] = 0 or 1`
2. **prefix_sum.slang** — subgroup-level prefix + one `atomicAdd` per workgroup (no second dispatch)
3. **scatter.slang** — GPU interpolation (lerp current/previous InstanceBuffer), writes compacted SoA + `DrawArgs.instanceCount`

**Dirty bit tracking:** Double-buffered cumulative dirty bit array. Logic ORs bits atomically as entities are modified. Render swaps front/back once per frame and uploads current state for all set bits. If render falls behind, it SIMDs OR dirty flags from all intermediate frames before processing (12.5KB bit array for 100K entities, fits in L1).

**5 InstanceBuffers** decouple Logic from VSync — breaks the chain `VSync → GPU holds buffer → CPU render thread blocks → holds slab read locks → Logic stalls`.

**Status:** Dirty bit tracking is functional. Dirty-bit-driven GPU upload path is designed but not yet wired (currently full-slab copy).

---

## Speculative Presentation & Anti-Events

Effects fire at prediction time, not server confirmation time. A ring buffer of `TemporalEvents` (audio triggers, particle spawns, VFX handles) maps simulation events to presentation handles, timestamped by logic frame.

On rollback, a `PresentationReconciler` diffs the abandoned timeline against the corrected one:
- **Orphaned events** (predicted but wrong): issue an **Anti-Event** (RapidFadeOut, SoftCancel, RapidDecay)
- **New events** (in corrected timeline but absent from prediction): play now
- **Matching events**: no action

Anti-Events fade/decay over ~20ms rather than instant cull — visually continuous across rollback.

**Status:** Designed. Not yet implemented. The audio wrapper must expose handle-based fade control to be compatible with this system.

---

## Roadmap

Two stages: **Foundation** (add remaining subsystems) then **Hardening** (lock down the substrate). See `docs/STATUS.md`
for the authoritative status tracker.

### Stage 1: Foundation (current)

1. **Editor (bare-bones)** — In progress. Scene hierarchy, entity inspection, reflected properties, save/load. ImGui
   docking, JSON serialization. Scope is explicitly limited to this definition.
2. **Networking** — GNS wrapper, client/server authority model, PIE loopback. Rollback netcode uses existing Temporal
   slab + Jolt snapshot.
3. **Audio** — SDL3 thin wrapper first (handle-based for Anti-Event compatibility). Minimal — just enough for gameplay
   feedback.

### Stage 2: Hardening

Once Editor + Networking + Audio are functional, the engine enters a dedicated cleanup, refactoring, and rewrite phase.
The goal is to make the substrate as solid as possible before building the gameplay layer (Construct/View system,
behavior trees, etc.) on top of it.

Targets include: dirty-bit GPU upload, Archetype/TemporalComponentCache deduplication, Fixed32/SimFloat, hot-path audit,
constraint system, static entity tier, reflection robustness, `TNX_STRIP_NAMES` build option.

**After hardening:** Arena shooter test level to prove the full stack.

### Designed, Not Yet Implemented

- Construct/View OOP layer (Construct<T>, Owned<T>, ConstructBatch, TickGroup, View family, defrag listeners)
- Cumulative dirty bit array wired to GPU upload
- `GetTemporalFieldWritePtr` migrated from Archetype to TemporalComponentCache
- `TemporalFrameStride` removed from Archetype (duplicated state)
- Presentation Reconciler (Anti-Events, speculative presentation diff)
- Fixed-point coordinate system (Fixed32, SimFloat alias, Jolt bridge validation)
- ConstraintEntity system (constraint pool, rigid attachment pass, physics root determination)
- Static entity tier (needs asset importing online first)

---

## Key Files

| Path | Purpose |
|------|---------|
| `src/Runtime/Core/Public/FieldProxy.h` | Core SoA field wrapper (Scalar/Wide/WideMask) |
| `src/Runtime/Core/Public/SchemaValidation.h` | Compile-time component validation |
| `src/Runtime/Core/Public/TemporalComponentCache.h` | N-frame SoA ring buffer (proto-History Slab) |
| `src/Runtime/Memory/` | Archetype chunks, cold component storage |
| `src/Runtime/Physics/` | Jolt integration (JoltJobSystemAdapter, JoltBody) |
| `src/Runtime/Rendering/` | VulkanContext, VulkanMemory, VulkRender |
| `shaders/` | Slang compute shaders (predicate, prefix_sum, scatter) |
| `docs/ARCHITECTURE.md` | Full architecture reference |
| `docs/PERFORMANCE_TARGETS.md` | Benchmark targets and testbed results |

---

## What Claude Should Know

- **Do not suggest replacing fixed-update with variable timestep.** The 512Hz fixed rate is a load-bearing architectural constraint, not an oversight.
- **Do not suggest using `std::map` or `std::unordered_map` in hot paths.** This codebase is latency-sensitive and data-oriented by design.
- **The OOP layer is intentional.** Constructs (`Construct<Player>`, `Construct<GameMode>`) are first-class citizens,
  not a facade to be removed. Construct authors should not need to understand SoA decomposition. Views handle all ECS
  interaction transparently.
- **Jolt is a temporary physics backend.** The constraint system is designed to be solver-agnostic. Don't suggest architectural changes that couple more tightly to Jolt.
- **The editor scope is intentionally limited.** Scene hierarchy + entity placement + reflected property inspection + save/load. Do not suggest expanding it.
- **Serialization:** JSON for all builds during R&D. Binary format will be introduced when it makes sense, not before.
  The engine includes a minimal hand-rolled JSON parser (`Json.h`) — intentionally not a vendored library. Swap-ready
  API if needs outgrow it.
- **R&D codebase.** Some areas are highly optimized to test a theory; others are deliberately left rough pending the cleanup pass. The dichotomy is intentional.