# Current Status (2026-04)

> **Navigation:
** [← Back to README](../README.md) | [← Configuration](CONFIGURATION.md) | [Game Flow →](FLOW.md) | [Networking →](NETWORKING.md)

---

## Timeline Context

**Project Start:** ~2026-02-01 (Week 1)
**Current Date:** 2026-04-03
**Phase:** Game Flow (Foundation Stage 3 — Construct/View + Networking proven, building gameplay layer)

---

## Roadmap

The roadmap is organized into two stages: **Foundation** and **Hardening**. The foundation stage adds the remaining
subsystems needed to build a real game. The hardening stage locks down the substrate before gameplay layer development
begins.

### Stage 1: Foundation (current)

| # | Milestone               | Status      | Notes                                                                                                                                                                            |
|---|-------------------------|-------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 1 | **Editor (bare-bones)** | Complete    | Scene hierarchy, entity inspection, reflected properties, save/load. ImGui docking, 6 panels. JSON serialization.                                                                |
| 2 | **Construct/View OOP**  | Complete    | `Construct<T>`, `Owned<T>`, `ConstructView<TEntity>`, `ConstructBatch` tick dispatch. PlayerConstruct proven with JoltCharacter.                                                 |
| 3 | **Networking**          | In Progress | GNS wrapper, PIE loopback, entity replication, clock sync, client input routing. Soul + NetChannel + SpawnRequest pipeline in progress. See [NETWORKING.md](NETWORKING.md).      |
| 4 | **Audio**               | Not started | SDL3 thin wrapper first (handle-based for Anti-Event compatibility). Minimal — just enough for gameplay feedback.                                                                |
| 5 | **Game Flow**           | In Progress | FlowManager, FlowState, GameMode, Soul, NetChannel implemented. Audio not started. ModeMixin system, WithSpawnManagement, SpawnRequest pipeline pending. See [FLOW.md](FLOW.md). |

### Stage 2: Hardening

Once Editor + Networking + Audio are functional, the engine enters a dedicated cleanup, refactoring, and rewrite phase.
The goal is to make the substrate as solid as possible before building behavior trees, AI directors, and higher-level
gameplay systems on top of the proven Construct/View foundation.

**Hardening targets (non-exhaustive):**

- ~~Wire cumulative dirty bit array to GPU upload~~ ✅ Implemented (2026-03-29)
- Migrate `GetTemporalFieldWritePtr` from Archetype to TemporalComponentCache
- Remove duplicated `TemporalFrameStride` from Archetype
- Fixed-point coordinate system (`Fixed32`, `SimFloat` alias, Jolt bridge validation)
- Audit hot-path data structures for cache efficiency
- Archetype field allocation and meta storage cleanup
- ConstraintEntity system (constraint pool, rigid attachment pass, physics root determination)
- Static entity tier (needs asset importing)
- Evaluate reflection system robustness (static init ordering, precompile step)
- Registration name strings strippable via build option (`TNX_STRIP_NAMES`) for shipping builds
- Default identity quaternion for CTransform (RotQw=1.0f) — prevent zero-quaternion rendering bugs

**After hardening:** Arena shooter test level to prove the full stack — high entity counts, physics, competitive input
latency, rollback netcode, networked multiplayer.

---

## Completed Work

### Foundation & Trinity Architecture

**Threading Model:**
- Trinyx Trinity (Sentinel/Brain/Encoder) fully operational
- Sentinel runs 1000Hz SDL event loop, owns VulkanContext + VulkanMemory
- Brain runs 512Hz fixed-timestep loop via accumulator (std::atomic<double>)
- Encoder (RendererCore/GameplayRenderer) functional — full GPU loop rendering entity data from TemporalComponentCache

**Raw Vulkan Stack:**
- Migrated from SDL3 GPU backend to raw Vulkan (volk 1.4.304 + VMA 3.3.0)
- VulkanContext + VulkanMemory migrated to vk::raii::
- VulkanContext: instance, device, swapchain, queues, sync primitives
- VulkanMemory: VMA allocator lifetime, buffer/image allocation helpers
- RendererCore Steps 1–4 complete: clear → indexed cube pipeline → GpuFrameData + BDA draw → entity data from
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
- CJoltBody volatile component (TNX_VOLATILE_FIELDS, CacheTier::Volatile) — SoA in slab, slab-direct iteration
- FlushPendingBodies: iterates contiguous DUAL+PHYS slab region directly via GetPhysicsRange(), no archetype/chunk
  indirection
- PullActiveTransforms: writes pos+rot from awake bodies back into SoA WriteArrays
- Physics loop: PrePhysics → (FlushPendingBodies → Jolt Step -- phystick % 0) → (PullActiveTransforms -- phystick %
  phystick - 1) → PostPhysics
- BodyID↔EntityCacheIndex bidirectional lookup (EntityToBody, BodyToEntity vectors)
- GetPhysicsRange() on ComponentCacheBase: returns [DualStart, PhysEnd) in cache index units for dense physics iteration
- JoltCharacter: CharacterVirtual wrapper for Construct-driven character controllers. Capsule shape, ExtendedUpdate
  (grounding, stair step, slope slide). Independent of CJoltBody — Constructs own and update directly via PhysicsStep
  tick

**Input System:**

- Double-buffered input with lock-free polling on Sentinel thread (1000Hz)
- Event buffer + bitstate for flexible querying (event-driven or polled)

**Math Libraries:**

- VecMath.h: VecLocal<N,WIDTH>, Vec2/3/4Accessor (reference-based, embedded in components)
- QuatMath.h: QuatLocal<WIDTH>, QuatAccessor
- FieldMath.h: Read, Clamp, Min, Max, Abs, Lerp, Step, SmoothStep

**Construct/View OOP Layer (2026-03 — 2026-04):**

- `Construct<T>` CRTP base — lifecycle management, auto-tick registration via C++20 concept detection
- Tick hooks: PrePhysics, PostPhysics, PhysicsStep, ScalarUpdate (zero-cost if not implemented)
- `ConstructBatch` — type-erased, non-virtual tick dispatch. Stable-sorted by (TickGroup, OrderWithinGroup)
- `TickGroup` enum: PreInput → Default → PostDefault → Camera → Late (fixed engine order)
- `Owned<T>` — value-member composition for child Constructs. Zero allocation, reverse-declaration-order cleanup
- `ConstructView<TEntity>` — generic view template, creates a backing ECS entity of any EntityView type,
  auto-rehydrates FieldProxy cursors on frame advance and defrag. Partition auto-derived from entity's components.
- `ConstructRegistry` — type-erased registry of live Constructs, deferred destruction
- `CameraConstruct` — in-world camera (LogicView, yaw/pitch/FOV state), swappable ActiveCamera on LogicThread
- `JoltCharacter` — Jolt CharacterVirtual wrapper for Construct-driven character controllers (capsule shape,
  grounding, stair stepping, slope sliding). Decoupled from JoltBody component — no Jolt body in the ECS.
- `JoltLayers` — shared header for Jolt object/broadphase layer constants (extracted from JoltPhysics.cpp)
- `JoltPhysics::GetTempAllocator()` — public accessor for passing TempAllocator to JoltCharacter::Update
- PlayerConstruct proven: ConstructView + JoltCharacter + 2 Owned CameraConstructs + WASD/mouse look
- Component renames: ColorData→CColor, Forces→CForces, JoltBody→CJoltBody, MeshRef→CMeshRef,
  RigidBody→CRigidBody, Rotation→CRotation, Scale→CScale, ShapeData→CShape, TransRot→CTransform,
  Translation→CTranslation
- Entity types: EInstanced (full DUAL: transform+physics+mesh+color+scale),
  EPlayer (DUAL without JoltBody: transform+mesh+color+scale), EPoint (minimal)
- Fix: `GetOrCreateArchetype` now passes `ClassSystemID` to `BuildLayout` — entities created at runtime
  via ConstructView get correct partition placement (was defaulting to LOGIC partition)

**Game Flow (2026-04 — in progress):**

- `FlowManager` — state stack manager, travel primitives, World/Level/Mode lifetime management
- `FlowState` — base class for flow states (menu, loading, gameplay, post-match). Declares requirements
  via `GetRequirements()` returning `StateRequirements{NeedsWorld, NeedsLevel, NeedsNetSession}`. FlowManager
  enforces requirements during transitions.
- `GameMode` — server-authoritative rules runtime. One per World. Inheritable base (not CRTP). Users
  opt into Construct ticks via multiple inheritance when needed.
- `Soul` — session-scoped player identity (not a Construct). Created by FlowManager::OnClientLoaded,
  destroyed by OnClientDisconnected. Holds OwnerID, InputLead, ConfirmedBodyHandle, NetChannel.
- `NetChannel` — typed per-connection send wrapper. Implemented in `NetChannel.h`.
- Travel toolbox model — three orthogonal levers (domain lifetime, Construct lifetime, network continuity)
  instead of a single travel policy. Games compose primitives to build their flow.
- Persistent Constructs survive World resets via reinitialization mechanism.
- Bootstrap contract: engine loads a single user-specified default state by name; user code owns the
  entire flow graph from there.
- Vocabulary: State (drives app), Mode (drives match), Level (content chunk)

**Networking (2026-03 — in progress):**

- `GNSContext` — GameNetworkingSockets wrapper (header isolation, static link)
- `NetConnectionManager` — server/client socket API, Listen/Connect, per-connection state (RTT, OwnerID, sequence
  tracking)
- `NetThread` — dedicated network poller at NetworkUpdateHz (default 30Hz), routes InputFrame messages to correct World
- `ReplicationSystem` — server-side entity replication:
    - Walks Registry each net tick, sends EntitySpawn (reliable) for new entities
    - Sends batched StateCorrection (unreliable) with authoritative transforms
    - `RegisterEntity()` pre-assigns EntityNetHandle with OwnerID before replication
    - `HandleEntitySpawn()` / `HandleStateCorrections()` — client-side entity creation and correction
- `EntityNetHandle` — packed uint32 (NetOwnerID:8 + NetIndex:24) for network identity
- `World` abstraction — self-contained simulation (Registry + Physics + Logic + Input + SpawnSync)
- PIE loopback: server + N client Worlds in same process, loopback GNS connections
- Focus-based input routing: `InputTargetWorld` pointer routes PumpEvents to focused PIE viewport
- Known gaps: no delta compression, no interest management, no entity destruction replication, no ownership camera

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
- [x] Raw Vulkan: VulkanContext, VulkanMemory (volk + VMA), RendererCore/GameplayRenderer (Encoder thread)
- [x] FieldProxy (Scalar / Wide / WideMask, FieldProxyMask zero-size base)
- [x] TemporalComponentCache SoA ring buffer (ComponentCacheBase / ComponentCache<Tier>, Volatile=3 frames, Temporal=N
  frames)
- [x] Dirty bit tracking (Active = 1<<31, Dirty = 1<<30, DirtiedFrame = 1<<29)
- [x] Registry dirty bit marking after each chunk update
- [x] Dirty-bit-driven selective GPU upload (RenderAck handshake, per-slab bitplanes, AVX2 scan)
- [x] LogicThread::PublishCompletedFrame (Vulkan RH perspective + identity view)
- [x] GPU-driven compute pipeline (predicate → prefix_sum → scatter → build_draws → sort_instances, Slang shaders)
- [x] InstanceBuffer SoA + indirect draw (DrawArgs)
- [x] RendererCore: clear → indexed cube → GpuFrameData + BDA draw → entity data from TemporalComponentCache
- [x] 3-level Tracy profiling (Coarse/Medium/Fine)
- [x] `TNX_TEMPORAL_FIELDS` / `TNX_VOLATILE_FIELDS` with SystemGroup tag (drives entity group auto-derivation)
- [x] `TemporalFlags` with Active/Dirty/DirtiedFrame/Alive/Replicated bits
- [x] Lock-free job system (MPMC ring buffers, futex-based wake, per-chunk dispatch)
- [x] Core-aware thread pinning (physical cores first, SMT siblings second)
- [x] GameManager CRTP pattern (`TNX_IMPLEMENT_GAME` macro)
- [x] Project-relative INI config (`*Defaults.ini` scanning from source directory)
- [x] Tiered storage partition layout (Cold/Static/Volatile/Temporal with dual-ended arena layout)
- [x] 5 GPU InstanceBuffers (cycling independently of 2 GPU frame-in-flight slots)
- [x] Jolt Physics v5.5.0 (JoltJobSystemAdapter, CJoltBody volatile component, slab-direct
  FlushPendingBodies/PullActiveTransforms)
- [x] Cold component infrastructure (TNX_REGISTER_FIELDS → CacheTier::None → SoA in chunk)
- [x] Input buffering (double-buffered, lock-free polling, event + bitstate querying)
- [x] VecMath / QuatMath / FieldMath libraries
- [x] Component validation: no vtable + all fields must be FieldProxy (SchemaValidation.h)
- [x] **Construct/View OOP layer** — `Construct<T>`, `Owned<T>`, `ConstructView<TEntity>`, `ConstructBatch`,
  `TickGroup`, JoltCharacter, defrag listeners, spawn handshake integration (2026-04)
- [x] **Rollback substrate** — LogicThread ExecuteRollback/ExecuteRollbackTest, JoltPhysics SaveSnapshot/RestoreSnapshot
  ring buffer (TNX_ENABLE_ROLLBACK). Byte-perfect ECS + Jolt determinism verified (2026-03-29).
- [x] **Game Flow (partial)** — FlowManager state stack + travel primitives, FlowState base class, GameMode base class,
  Soul class (OwnerID identity, ClaimBody/ReleaseBody, RPC dispatch), NetChannel typed send wrapper,
  ConstructRegistry lifetime-bucketed storage (Level/World/Session/Persistent) (2026-04)
- [x] **Networking** — GNS, GNSContext, NetConnectionManager, NetThreadBase (
  ClientNetThread/ServerNetThread/PIENetThread),
  entity spawn replication, ConstructSpawn replication, state corrections, PIE loopback,
  PlayerInputLog per-player ring buffer, clock sync, Soul RPC system (PlayerBegin/Confirm/Reject) (2026-04)

### Not Yet Implemented

- [ ] **Fixed-point coordinate system** — `Fixed32` value type (1 unit = 0.1mm), `FieldProxy<Fixed32, WIDTH>`, render thread conversion to camera-relative float32 at upload, Jolt bridge (int32↔float32 at cell boundary)
- [ ] **ConstraintEntity system** — constraint entities in LOGIC partition, `ConstraintType` enum (Rigid/Hinge/BallSocket/Prismatic/Distance/Spring), render thread rigid attachment pass (2-pass depth ordering), physics root determination
- [ ] **Space partition cell registry** — cell world origins as float64/int64, cell assignment at entity spawn, cross-cell reparenting
- [ ] **Presentation Reconciler** — Anti-Events (RapidFadeOut, SoftCancel, RapidDecay) for rollback-driven effect
  correction
- [ ] **Audio** — SDL3 thin wrapper (handle-based for Anti-Event compatibility)
- [ ] `GetTemporalFieldWritePtr` migration from Archetype to TemporalComponentCache
- [ ] `TemporalFrameStride` removal from Archetype (duplicated from cache)

### Known Issues / Technical Debt

1. **TemporalFrameStride duplicated on Archetype:** Should call `cache->GetFrameStride()` instead.
   Currently cached redundantly on every Archetype.

### Planned (Next Phase)

- [x] **Render pipeline optimization** — dirty-bit selective GPU upload (2026-03-29)
- [x] **Rollback determinism** — full slab rollback + Jolt SaveState/RestoreState, byte-perfect ECS + Jolt determinism
  verified (2026-03-29)
- [x] **Construct/View OOP layer** — full stack proven with PlayerConstruct (2026-04)
- [x] **Basic networking** — GNS, entity spawn replication, state corrections, PIE loopback (2026-04, in progress)
- [ ] **Delta compression** — send only changed fields instead of full state corrections
- [ ] **Owned entity camera** — client detects its owned entity via NetOwnerID, attaches follow camera
- [ ] **Frustum culling** — SIMD 6-plane test, GPU-side predicate enhancement
- [ ] **State-sorted rendering** — 64-bit sort keys, GPU radix sort after scatter
- [ ] **Rollback netcode** — network integration using proven rollback substrate + dirty resimulation

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

### Replay & Recording — Architecture Wins

The slab-based SoA layout means replay recording is nearly free. All deterministic simulation state already lives in
contiguous, trivially-copyable field arrays within the slabs. Adding replay is just serialization in
`PropagateFrame` — no new infrastructure required.

**Compression:** SoA field arrays compress exceptionally well. Homogeneous float arrays (all `PosX`, all `PosY`, etc.)
have high spatial coherence. Delta compression between frames yields mostly zeros since most entities are
near-stationary
per tick. Far superior to AoS replay formats where mixed types destroy compression ratios.

**Free wins from existing architecture:**

- **Server & client replay** — Same slab data, same serialization path, deterministic output regardless of recorder
- **Kill cam / rewind** — Slab snapshots already in a ring buffer; rewind is just indexing backward and re-rendering
  with a different camera target
- **Spectator scrubbing** — Random access seek to any frame via snapshot index, resume simulation forward
- **Anti-cheat validation** — Diff server vs client slab timelines; any divergence flags cheating
- **Bandwidth estimation** — Compressed delta size between frames represents the theoretical minimum sync payload for
  netcode

**Sub-tick event replay:** Input events are timestamped at actual ms read time (sub-tick precision). Replay files
preserve
exact event timing within frames, not just frame-quantized approximations. Combined with deterministic resim, replays
reproduce identical outcomes.

**With rollback enabled:** Retroactive event insertion from replay data enables replaying remote client events at their
original timestamps and resimming forward. Correct kill attribution across latency gaps. Frame-perfect reproduction of
multiplayer sessions from any participant's perspective.

---
