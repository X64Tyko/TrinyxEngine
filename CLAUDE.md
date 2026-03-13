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

**Requirements (Linux):** CMake 3.20+, C++20 compiler, SDL3 system package (`libsdl3-dev`), Tracy submodule (`libs/tracy`).

```bash
# Standard development build (RelWithDebInfo recommended for profiling)
cmake -B cmake-build-relwithdebinfo -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build cmake-build-relwithdebinfo

# Run the testbed
./cmake-build-relwithdebinfo/Testbed/Testbed

# Debug build
cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug
```

**Key CMake options** (append to the configure step):

|--------|---------|---------|
| Option | Default | Purpose |
|--------|---------|---------|
| `ENABLE_TRACY=ON/OFF` | ON | Tracy profiler integration |
| `TRACY_PROFILE_LEVEL=1/2/3` | 3 | 1=coarse (~1%), 2=medium (~5%), 3=per-entity (~50%+ overhead) |
| `ENABLE_AVX2=ON/OFF` | ON | `-march=native` on GCC/Clang |
| `GENERATE_ASSEMBLY=ON/OFF` | OFF | Emit `.s` files for vectorization inspection |
| `VECTORIZATION_REPORTS=ON/OFF` | OFF | Compiler loop-vectorization diagnostics |
| `TNX_ALIGN_64=ON/OFF` | OFF | 64-byte vs 32-byte field array alignment |

---

## Architecture Philosophy

Three interlocking design decisions define everything else:

1. **Fixed 512Hz logic update.** This is a constraint, not a limitation — If we can't improve the internet itself, we
   improve the software.

2. **Tiered SoA storage with rollback as a first-class citizen.** Hot component data lives in SoA ring buffers (Temporal: N-frame rollback history, Volatile: triple-buffer no rollback). Cold data lives in archetype chunks. The tier is determined by the component macro, not by the entity. An entity's effective tier is the highest tier of any of its components.

3. **OOP API over a data-oriented substrate.** Entity authors write `PlayerController`, `GameMode`, `GameState` —
   familiar concepts from Unreal. The engine decomposes those into SoA field arrays transparently. Developers need
   minimal understanding of the substrate to use it--DoD and ECS concepts, not a working knowledge; they only need to
   understand it if they're extending it.

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

## Entity Lifecycle

### Spawn

The calling thread provides a lambda and performs a synchronized handshake with the Logic thread at the top of its frame. The Logic thread allows the spawning thread to write new entity data, then the spawning thread signals Logic to continue. This is synchronous, thread-safe, and wraps the fundamental contract behind deferred, queued, and batched spawning variants.

### Despawn

Entity is requested for deletion → goes into a deletion queue → tombstoned (Active flag cleared) immediately. The tombstone propagates through all systems for free: GPU predicate pass stops drawing it, physics awake-only pull ignores it, 64-entity bitplane scan skips it. A deferred destroy then runs on the Logic thread to reclaim memory. Visual and physics despawn happen at tombstone time; memory reclamation happens at deferred destroy time. These are decoupled but aligned.

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

### Current Phase: Foundation Completion + Cleanup

The engine substrate is feature-complete for its stated goals. The cleanup pass is the gateway to the game layer — not an afterthought. Finish the foundation, clean the seams, then the OOP game layer becomes additive rather than surgical.

Specific cleanup targets: duplicated state between Archetype and TemporalComponentCache (`GetTemporalFieldWritePtr`, `TemporalFrameStride`), wiring the cumulative dirty bit array to the GPU upload path, and the Fixed32 / SimFloat alias system.

### Next Phase (in order)

1. **Editor (bare-bones)** — scene hierarchy, entity placement, reflected property inspection, save/load. JSON for
   development, binary for shipped games. Scope is explicitly limited to this definition.
2. **Arena shooter test level** — simple arena shooter to prove all fundamental ideologies: high entity counts, physics,
   competitive input latency, rollback netcode
3. **Data-driven spawn** — lambda + handshake model first (synchronous, proves the contract), deferred/batched variants
   after
4. **Rollback netcode** — Jolt snapshot strategy already decided, Temporal slab rollback + dirty resimulation
5. **Audio** — SDL3 thin wrapper first (handle-based for Anti-Event compatibility), custom layer later
6. **Cleanup pass** — remove duplicated state, wire dirty-bit GPU upload, audit hot-path data structures, fix Jolt push
   logic, archetype field allocation and meta storage

### Designed, Not Yet Implemented

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
- **The OOP layer is intentional.** `PlayerController`, `GameMode`, `GameState` etc. are first-class citizens, not a facade to be removed. Entity authors should not need to understand SoA decomposition.
- **Jolt is a temporary physics backend.** The constraint system is designed to be solver-agnostic. Don't suggest architectural changes that couple more tightly to Jolt.
- **The editor scope is intentionally limited.** Scene hierarchy + entity placement + reflected property inspection + save/load. Do not suggest expanding it.
- **Serialization:** JSON for development/editor, binary for shipped games. This decision is final.
- **R&D codebase.** Some areas are highly optimized to test a theory; others are deliberately left rough pending the cleanup pass. The dichotomy is intentional.