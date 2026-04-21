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

| Option                         | Default | Purpose                                                                                                    |
|--------------------------------|---------|------------------------------------------------------------------------------------------------------------|
| `TNX_ENABLE_EDITOR=ON/OFF`     | OFF     | Enable editor UI (ImGui + GPU picking)                                                                     |
| `TNX_ENABLE_ROLLBACK=ON/OFF`   | OFF     | Enable N-frame rollback history for netcode (forces `TNX_DETERMINISM=ON`)                                  |
| `TNX_DETERMINISM=ON/OFF`       | OFF     | Cross-platform determinism: disables FMA/fast-math in engine and Jolt (`JPH_CROSS_PLATFORM_DETERMINISTIC`) |
| `ENABLE_TRACY=ON/OFF`          | ON      | Tracy profiler integration                                                                                 |
| `TRACY_PROFILE_LEVEL=1/2/3`    | 3       | 1=coarse (~1%), 2=medium (~5%), 3=per-entity (~50%+ overhead)                                              |
| `ENABLE_AVX2=ON/OFF`           | ON      | `-march=native` on GCC/Clang                                                                               |
| `GENERATE_ASSEMBLY=ON/OFF`     | OFF     | Emit `.s` files for vectorization inspection                                                               |
| `VECTORIZATION_REPORTS=ON/OFF` | OFF     | Compiler loop-vectorization diagnostics                                                                    |
| `TNX_ALIGN_64=ON/OFF`          | OFF     | 64-byte vs 32-byte field array alignment                                                                   |
| `TNX_DETAILED_METRICS=ON/OFF`  | OFF     | Per-frame latency breakdown logging                                                                        |

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
Arena 1: Renderable  [0 .. MAX_RENDERABLE_ENTITIES)
  RENDER (→) starts at 0              — render-only (particles, decals, ambient props)
  DUAL   (←) starts at MAX_RENDERABLE — physics + render (players, AI, physics props)

Arena 2: Cached  [MAX_RENDERABLE_ENTITIES .. MAX_CACHED_ENTITIES)
  PHYS  (→) starts at MAX_RENDERABLE — physics-only (triggers, invisible movers)
  LOGIC (←) starts at MAX_CACHED     — logic/rollback-only entities
```

Physics iterates DUAL + PHYS contiguously at the arena boundary — a dense wall of 100% relevant entities, no gap to
skip. Render iterates RENDER + DUAL with a gap in Arena 1; the GPU predicate pass handles gaps at negligible cost. Jolt
body arrays are sized by `MAX_JOLT_BODIES` (separate from the arena boundary).

### Entity Group Auto-Derivation

The partition group (Dual/Phys/Render/Logic) is computed automatically from `SystemGroup` tags on components at both
`TNX_REGISTER_ENTITY` time and runtime entity creation via `ConstructView`. The `ClassSystemID` is passed to
`BuildLayout` in both `InitializeArchetypes` and `GetOrCreateArchetype`. There is no manual group annotation — it would
be a footgun that shoots gaps into partition regions.

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

The active strip is scanned 64 entities at a time (one 64-bit word). A zero word (64 inactive entities) is skipped with
no field data touched. This covers the gap between RENDER and DUAL buckets in Arena 1 at negligible overhead. For mixed
words, `FieldProxy` uses AVX2 masked loads/stores (8-wide, branchless).

---

## Gameplay Object Model — Constructs & Entities

### Two Object Types

**Constructs** are singular, complex scalar objects — the things that think. A Player, a GameMode, a TurretBase, an
AIDirector. They inherit from `Construct<T>` (CRTP) and compose Views to access ECS data. Created via
`ConstructRegistry::Create<T>(world)`.

**Entities** are raw ECS data for the horde — zombies, bullets, debris, particles. No bespoke logic. The engine sweeps
them with wide SIMD. Registered via `TNX_REGISTER_ENTITY`.

### Construct<T>

CRTP base that owns the full lifecycle. Auto-registers ticks via `if constexpr` concept detection — implement the
method, get the tick. Don't implement it, pay nothing.

```cpp
class Player   : public Construct<Player>   { ConstructView<EPlayer> Body; ... };
class GameMode : public Construct<GameMode>  { /* no View — pure logic */ };
```

### Composition via Owned<T>

Complex Constructs compose via `Owned<T>` value members — compile-time ownership, deterministic init/destroy order,
automatic lifetime management:

```cpp
class Turret : public Construct<Turret>
{
    ConstructView<EInstanced> Body;     // Physics + render in DUAL partition
    Owned<BarrelAssembly>  Barrel;      // has its own ConstructView, own tick
    Owned<TargetingSystem> Targeting;   // pure logic, no View needed
    Owned<AmmoFeed>        Ammo;        // data-only, no physics
};
```

Each owned Construct is exactly as heavy as it needs to be. `TargetingSystem` doesn't pay for a physics body. `AmmoFeed`
doesn't tick unless it implements a tick method. C++20 concepts on `Owned<T>` enable compile-time interface contracts (
`Targetable`, `Damageable`) — zero-cost replacement for runtime gameplay tag queries.

### ConstructView<TEntity>

`ConstructView<TEntity>` is a generic template that creates a backing ECS entity of any EntityView type, hydrates
FieldProxy cursors on initialization, and auto-rehydrates when the write frame advances or defrag relocates the entity.
Partition is auto-derived from the entity type's component SystemGroup tags.

| Entity Type | Components                                          | Partition |
|-------------|-----------------------------------------------------|-----------|
| EInstanced  | CTransform + CJoltBody + CScale + CColor + CMeshRef | DUAL      |
| EPlayer     | CTransform + CScale + CColor + CMeshRef             | DUAL      |
| EPoint      | CTransform only                                     | PHYS      |

**Ownership chain:** Construct → ConstructView → ECS Entity → Components → FieldArrays → FieldProxies

### Tick Dispatch — ConstructBatch

Constructs register into typed scalar batches on the Brain thread. Dispatch is type-erased and non-virtual:

```cpp
struct ConstructTickEntry { void* Object; void (*Fn)(void*); TickGroup Group; int16_t OrderWithinGroup; };
```

TickGroup is a fixed engine enum controlling execution order:

```cpp
enum class TickGroup : uint8_t { PreInput=0, Default=1, PostDefault=2, Camera=3, Late=4 };
```

Brain thread tick sequence (per fixed step):

```
ProcessSimInput → PrePhysics wide sweep → ScalarPrePhysics batch (Constructs)
  → FlushPendingBodies → PushKinematicTransforms → ScalarPhysicsStep batch (Constructs)
  → Jolt Step (async) → PullActiveTransforms →
PostPhysics wide sweep → ScalarPostPhysics batch (Constructs)
  → PublishCompletedFrame → PropagateFrame
```

Scalar update runs outside the fixed loop when time permits:

```
ScalarUpdate wide sweep → ScalarUpdate batch (Constructs — camera, UI, post-logic)
```

The ScalarPhysicsStep slot is where Constructs drive JoltCharacter or write kinematic targets.
The Entity ScalarUpdate slot is the bridge point: an AIDirector Construct thinks once, then writes
target positions into zombie entity fields. The zombie has no bespoke logic — it's swept by the
wide PrePhysics pass.

### Serialization

Constructs do NOT serialize their own C++ members. Only View-owned ECS data is serialized. If a value is
designer-authored (e.g., MaxAmmo), it belongs in a component so it serializes through the existing ECS path. No
special-case codepath.

### Thread Safety

Tick registration follows the spawn handshake contract: the handshake window is the one safe place to mutate engine
state. Registration outside the window is deferred to the next handshake. One pattern for all state mutation.

---

## Game Flow — States, Modes & Travel

Three concepts drive the application above the ECS layer:

- **State** — flow state, drives the app (menu, loading, gameplay, post-match)
- **Mode** — rules runtime, drives the match (server authority while in-game)
- **Level** — content chunk (`.tnxscene`)

### FlowManager

Owned by `TrinyxEngine`, runs on Sentinel thread. Manages a `GameState` stack and orchestrates World/GameMode/Level
lifetimes based on each state's declared `StateRequirements` (NeedsWorld, NeedsLevel, NeedsNetSession).

Bootstrap: `flow.RegisterState("name", factory)` in `PostInitialize`, then `flow.LoadDefaultState("MainMenu")`.
User code owns the entire flow graph from there.

### Travel: Composable Primitives

Travel is three orthogonal levers, not a single policy:

- **Lever A — Domain lifetime:** Keep World + Swap Level | Reset World | Keep nothing
- **Lever B — Construct lifetime:** Persistent (survives everything via reinitialization) | World-scoped | Level-scoped
- **Lever C — Network continuity:** Keep NetSession | Swap NetSession

### GameState

Virtual base class. `OnEnter`/`OnExit`/`Tick` hooks. Declares requirements — FlowManager enforces them during
transitions (creates World if needed, destroys if not).

### GameMode

Inheritable virtual base class (NOT CRTP, NOT Construct). Users that need per-frame ticks also inherit Construct<T>:
`class ArenaMode : public GameMode, public Construct<ArenaMode>`. Pure event-driven modes skip Construct overhead.

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

**Networked despawn — four phases (designed, not yet implemented):**

- **Phase 0 — Tentative:** entity dies in speculative sim → recorded in `TentativeDestroys[frame]` on the per-client `ServerClientChannel`. Net index held. No packet sent. Rollback cancels this and revives the entity.
- **Phase 1 — Commit:** `CommittedFrameHorizon` advances past the death frame (all player inputs confirmed) → entry graduates to `PendingNetDespawns`. `Replicated[i]` cleared on the channel. The server cannot free the net slot before this transition.
- **Phase 2 — Send:** `SendDespawns()` batches N × `uint32_t` net handle values, sends reliable. `ConfirmNetRecycles()` fires after send. GNS reliable ordering makes explicit ACK unnecessary.
- **Phase 3 — Client Apply:** `OwnerNetThread` `EntityDestroy` handler receives the batch, looks up each handle via `NetToRecord`, calls `Destroy()` locally.

Wire format: `EntityDestroyPayload` = N × `uint32_t` (net handle values). Count = `PayloadSize / 4`.

---

## Physics: Jolt Integration

Jolt Physics v5.5.0 runs at a configurable fraction of the logic rate. Default: 512Hz logic / 64Hz physics (8:1 ratio). The ratio is configurable — a game needing high physics accuracy can set it 1:1.

**Key design decisions:**

- **Jolt owns physics state.** The ECS does not push state to Jolt every frame. Jolt's integrator advances bodies internally. ECS only writes to Jolt on explicit overrides (spawn, teleport, impulse, kinematic target).
- **Awake-only pull.** After each Jolt step, only awake bodies are pulled back into SoA WriteArrays. A scene with 50K bodies where 200 are moving pays for 200 pulls.
- **Only transforms are pulled.** Velocities stay in Jolt. Gameplay logic that needs velocity queries Jolt directly during ScalarUpdate.

### JoltCharacter (CharacterVirtual Wrapper)

`JoltCharacter` wraps `JPH::CharacterVirtual` for Construct-driven character controllers. It is completely
independent of the `CJoltBody` component — no Jolt body is created in the ECS. The Construct owns the
JoltCharacter directly and drives it through the `PhysicsStep` tick:

```cpp
class Player : public Construct<Player>
{
    ConstructView<EPlayer> Body;
    JoltCharacter CharacterController;

    void InitializeViews() {
        Body.Initialize(this);
        CharacterController.Initialize(
            GetWorld()->GetPhysics()->GetPhysicsSystem(),
            JPH::RVec3(0, 5, 0), 0.3f, 0.7f);
    }

    void PhysicsStep(SimFloat dt) {
        CharacterController.Update(desiredVelocity, gravity, dt,
            *GetWorld()->GetPhysics()->GetTempAllocator());
        // Write resolved position back to slab via FieldProxy
        JPH::RVec3 pos = CharacterController.GetPosition();
        Body.Transform.PosX = pos.GetX();
        Body.Transform.PosY = pos.GetY();
        Body.Transform.PosZ = pos.GetZ();
    }
};
```

`JoltLayers.h` provides shared layer constants (Static, Dynamic) used by both JoltPhysics and JoltCharacter.
`JoltPhysics::GetTempAllocator()` exposes the Jolt temp allocator for CharacterVirtual's ExtendedUpdate.

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

### Stage 1: Foundation

1. **Editor (bare-bones)** — Complete. Scene hierarchy, entity inspection, reflected properties, save/load. ImGui
   docking, JSON serialization. Scope is explicitly limited to this definition.
2. **Construct/View OOP** — Complete. `Construct<T>`, `Owned<T>`, `ConstructView<TEntity>`, `ConstructBatch`,
   JoltCharacter. PlayerConstruct proven.
3. **Networking** — Functional. GNS wrapper, Authority/Owner/Host model, PIE loopback, entity spawn replication,
   state corrections. Major refactor in progress: `LogicThread<TSimMode>`, `ServerClientChannel`, four-phase networked
   despawn, gated push replication model. Delta compression and rollback netcode pending.
4. **Audio** — Not started. SDL3 thin wrapper first (handle-based for Anti-Event compatibility).
5. **Game Flow** — In progress. FlowManager (state stack + travel primitives), GameState (flow states with declared
   requirements), GameMode (inheritable rules runtime). Toolbox travel model: three orthogonal levers (domain lifetime,
   Construct lifetime, network continuity) composable per-game. Persistent Constructs survive World resets via
   reinitialization. Bootstrap: engine loads one named default state, user code owns the flow graph.
6. **Camera System** — Designed. `CameraManager` owned by `Soul`. `CameraSlotStack[5]` (World/Gameplay/Tactical/Effect/Cinematic),
   per-slot multi-layer blending. `ECameraNode` (cold archetype, lobby/loading cameras). `ECamera` (hot SoA, Construct cameras).
   `CurveHandle` for transition curves.

### Stage 2: Hardening

Once the gameplay layer is proven with a test arena, the engine enters a dedicated cleanup, refactoring, and rewrite
phase. The goal is to make the substrate as solid as possible before building behavior trees, AI directors, and
higher-level systems.

Targets include: Fixed32/SimFloat, hot-path audit, constraint system, static entity tier, reflection robustness,
`TNX_STRIP_NAMES` build option.

**After hardening:** Arena shooter test level to prove the full stack.

### Designed, Not Yet Implemented

- ~~Construct/View OOP layer~~ ✅ Implemented (2026-04)
- ~~Cumulative dirty bit array wired to GPU upload~~ ✅ Implemented (2026-03)
- ~~`GetTemporalFieldWritePtr` migrated from Archetype to TemporalComponentCache~~ ✅ Done (2026-04)
- ~~`TemporalFrameStride` removed from Archetype (duplicated state)~~ ✅ Done (2026-04)
- `LogicThread<TSimMode>` CRTP refactor — `AuthoritySim`/`OwnerSim`/`SoloSim`, eliminates `std::function PlayerInputInjector`
- `ServerClientChannel` — per-client state: `PlayerInputLog`, `Replicated[]`, `TentativeDestroys`, `PendingNetDespawns`, `NetChannel`
- Four-phase networked despawn (Tentative → Commit → Send → Client Apply, gated on `CommittedFrameHorizon`)
- Gated push replication model (`OnFramePublished` → per-client read-only jobs, queued for next net tick)
- Camera system — `CameraManager`, `CameraSlotStack`, `CameraLayer`, `ECameraNode`, `ECamera`, `CurveHandle`
- Game Flow wiring (FlowManager state transitions, World create/destroy, level load/unload, GameMode lifecycle)
- Presentation Reconciler (Anti-Events, speculative presentation diff)
- Fixed-point coordinate system (Fixed32, SimFloat alias, Jolt bridge validation)
- ConstraintEntity system (constraint pool, rigid attachment pass, physics root determination)
- Static entity tier (needs asset importing online first)

---

## Key Files

| Path                                               | Purpose                                                          |
|----------------------------------------------------|------------------------------------------------------------------|
| `src/Runtime/Core/Public/FieldProxy.h`             | Core SoA field wrapper (Scalar/Wide/WideMask)                    |
| `src/Runtime/Core/Public/SchemaValidation.h`       | Compile-time component validation                                |
| `src/Runtime/Core/Public/TemporalComponentCache.h` | N-frame SoA ring buffer; `GetWriteFramePtr`/`GetReadFramePtr`    |
| `src/Runtime/Core/Public/LogicThread.h`            | Brain thread — being refactored to `LogicThread<TSimMode>`       |
| `src/Runtime/Core/Public/Types.h`                  | `EngineMode` enum (Standalone/Host/Authority/Owner), `SoulRole`  |
| `src/Runtime/Construct/Public/Construct.h`         | Construct<T> CRTP base, tick auto-registration                   |
| `src/Runtime/Construct/Public/ConstructView.h`     | ConstructView<TEntity> — generic ECS lens for Constructs         |
| `src/Runtime/Construct/Public/ConstructRegistry.h` | Type-erased Construct registry, deferred destruction             |
| `src/Runtime/Flow/Public/`                         | FlowManager, GameState, GameMode, Soul — game flow layer         |
| `src/Runtime/Memory/`                              | Archetype chunks, Registry, cold component storage               |
| `src/Runtime/Net/Public/AuthorityNetThread.h`      | Authority-side net handler (dedicated server / Host)             |
| `src/Runtime/Net/Public/OwnerNetThread.h`          | Owner-side net handler (client)                                  |
| `src/Runtime/Net/Public/ReplicationSystem.h`       | Entity replication — being refactored to ServerClientChannel     |
| `src/Runtime/Net/Public/NetChannel.h`              | Per-connection typed send wrapper                                |
| `src/Runtime/Net/Public/NetTypes.h`                | Wire types: `EntityNetHandle`, `EntitySpawnPayload`, `SoulRole`  |
| `src/Runtime/Physics/Public/JoltPhysics.h`         | Jolt integration (body management, step, pull)                   |
| `src/Runtime/Physics/Public/JoltCharacter.h`       | CharacterVirtual wrapper for Construct-driven controllers        |
| `src/Runtime/Physics/Public/JoltLayers.h`          | Shared Jolt layer constants (Static, Dynamic)                    |
| `src/Runtime/Entities/Public/`                     | Entity types: EInstanced, EPlayer, EPoint                        |
| `src/Runtime/Components/Public/`                   | Components: CTransform, CJoltBody, CColor, CScale, CMeshRef      |
| `src/Runtime/Rendering/`                           | VulkanContext, VulkanMemory, VulkRender                          |
| `shaders/`                                         | Slang compute shaders (predicate, prefix_sum, scatter)           |
| `docs/ARCHITECTURE.md`                             | Full architecture reference                                      |
| `docs/NETWORKING.md`                               | Networking architecture, ServerClientChannel, despawn design     |
| `docs/PERFORMANCE_TARGETS.md`                      | Benchmark targets and testbed results                            |

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
- **Networking vocabulary is locked.** Use only these terms — never "server" or "client" as standalone nouns:
  - `Authority` — dedicated server or the authoritative sim side of a Host
  - `Owner` — the local player; the Soul that owns input for an entity
  - `Host` — listen server; Soul with both `Authority + Owner` roles (`EngineMode::Host`)
  - `Echo` — non-owning entity stance on a client (a remote player's representation)
  - `Solo` — offline, no networking (`EngineMode::Standalone`, `SoloSim`)
  - `AuthorityNetThread` / `OwnerNetThread` — net handler class names
- **`ServerClientChannel` is the unit of per-client state.** It owns: `PlayerInputLog`, `Replicated[]`, `TentativeDestroys`, `PendingNetDespawns`, `NetChannel Send`, `ClientRepState`, `OwnerID`. Lives inside the World it belongs to for PIE isolation.
- **Networked despawn is four-phase**, gated on `CommittedFrameHorizon`. The server cannot free a net slot until the CommittedFrameHorizon passes the death frame. See `docs/NETWORKING.md` for the full design.
- **`LogicThread` is being templatized on `TSimMode`** (`AuthoritySim`/`OwnerSim`/`SoloSim`). Avoid adding new branching to `LogicThread` — it belongs in the sim mode instead.