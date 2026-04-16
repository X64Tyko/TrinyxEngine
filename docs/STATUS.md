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

#### Networking

1. **`Soul::Channel` is a dead stored member.** `Soul` holds a persistent `NetChannel` member, but
   `DispatchServerRPC` / `DispatchClientRPC` unconditionally overwrite it from `RPCContext` on every call.
   The stored value is never read between dispatches — the member is effectively dead. Correct fix: thread
   `NetChannel` through the dispatch call and have RPC thunks capture it from dispatch context, eliminating
   `GetNetChannel()` and the stored member entirely.

2. **`NetChannel` CI pointer can dangle in PIE.** `NetChannel` stores a raw `ConnectionInfo*`. If
   `NetConnectionManager::Connections` (a `std::vector`) reallocates on a new connection, all outstanding
   `NetChannel` objects hold dangling pointers. Fix: stable storage for `ConnectionInfo` (e.g. index-based
   or pointer-stable pool), or `NetChannel` stores an index + generation instead of a raw pointer.

3. **`FindConnectionByOwnerID` was ambiguous in PIE.** In PIE, two `ConnectionInfo` entries share the same
   OwnerID — one `bServerSide`, one `bClientInitiated`. The function now accepts a `requireServerSide` flag
   to disambiguate. All call sites must pass the correct flag; audit required if new call sites are added.

4. **`SendPong` builds its header manually, bypassing `MakeHeader`.** Every other send path goes through
   `NetChannel::MakeHeader` which stamps `LastAckedClientFrame`. `SendPong` was constructing the header by
   hand and omitting this field — fixed — but the pattern is a regression risk. Consider making `SendPong`
   call `MakeHeader` like all other sends.

5. **Heartbeat Ping reuses the clock-sync message type.** `TickReplication` sends a `NetMessageType::Ping`
   to propagate ACKs during quiet frames. This conflates ACK heartbeats with RTT measurement pings. A
   dedicated `NetMessageType::Ack` (header-only, no clock-sync semantics) would be cleaner.

6. **Souls do not exist in standalone mode.** The Soul creation path is gated behind the networking
   handshake. In standalone, no Souls are created — gameplay code that queries Souls simply finds nothing.
   Correct design: always create Souls (synthesise a local Soul per player during standalone World init,
   OwnerID from a local counter, no net session). This unifies the code path and enables local multiplayer
   without a divergent flow graph. `Soul::GetNetChannel()` must be null-safe when no CI exists.

7. **`PlayerInputLog::Store()` high-water guard is belt-and-suspenders.** `HighWaterFirstFrame` was stuck
   at 1 for the entire session because ACK trimming was broken (see items 3–4 above). The `Store()` loop
   is now clamped to `LastConsumedFrame - Depth + 1` regardless of ACK state, bounding it to O(ring_depth).
   Once ACK trimming is verified stable in production, the belt-and-suspenders clamp can be removed if
   desired — but it costs nothing to keep.

#### ECS / Memory

8. **`TemporalFrameStride` duplicated on Archetype.** Should call `cache->GetFrameStride()` instead.
   Currently cached redundantly on every Archetype instance.

9. **`GetTemporalFieldWritePtr` lives on Archetype, not TemporalComponentCache.** Logically belongs on the
   cache. Moving it is mechanical but touches several call sites — deferred to hardening pass.

10. **Reflection system relies on static initialisation order.** `TNX_REGISTER_COMPONENT`,
    `TNX_TEMPORAL_FIELDS`, `TNX_REGISTER_SCHEMA` etc. are all driven by static constructors. Cross-TU
    ordering is undefined in C++. Currently works because all registrations happen to resolve before
    `TrinyxEngine::Initialize()`, but this is fragile. A dedicated precompile step (like UBT) or an
    explicit registration call per module would be safer long-term.

#### Rendering

11. **Default identity quaternion not enforced on `CTransform`.** A zero quaternion (`RotQW=0`) is
    mathematically invalid and produces degenerate rendering. Spawned entities that don't explicitly set
    rotation will render incorrectly. Fix: initialise `RotQW=1.0f` in `CTransform`'s default state, or
    assert in the shader/scatter pass.

### Planned (Next Phase)

- [ ] **Delta compression** — send only changed fields instead of full state corrections
- [ ] **Owned entity camera** — client detects its owned entity via NetOwnerID, attaches follow camera
- [ ] **Frustum culling** — SIMD 6-plane test, GPU-side predicate enhancement
- [ ] **State-sorted rendering** — 64-bit sort keys, GPU radix sort after scatter
- [ ] **Rollback netcode** — network integration using proven rollback substrate + dirty resimulation

---

## Design Insights Log

A record of non-obvious decisions, the tradeoffs considered, and why the chosen path was taken.
Scattered rationale from other docs is consolidated here.

---

### Simulation Rate — 512Hz

The fixed update rate started at 512Hz as an ambitious target inspired by knowing that games like
Valorant run at 128Hz. Early profiling on the multithreaded job system showed that 256Hz was trivially
achievable, so the target doubled. The 1.95ms/frame budget is tight enough to force discipline but has
been met comfortably in testing at 100k+ entities.

The rate is a load-bearing architectural constraint, not a tunable config knob. Everything downstream
— input timestamping, physics divisor, rollback depth, clock sync — is designed assuming a stable,
known tick rate. Variable timestep for the authoritative simulation is incompatible with rollback
netcode and deterministic replay.

The engine does have a variable-rate update: the **Scalar Update** tick, which has existed since the
very beginning. Camera, cosmetic logic, UI, and non-authoritative Construct ticks run outside the
fixed loop at whatever rate the machine can sustain. This is an explicit, deliberate split: fixed
rate for everything that must be deterministic; variable rate for everything that only needs to look
smooth. The 512Hz fixed loop and the variadic scalar update are two halves of the same design, not
alternatives to each other.

---

### Why Three Threads, Not Two or Four

Two threads (logic + render) is the conventional split. The problem: render thread stalls on VSync
hold the slab read lock, which blocks logic. Three threads — Sentinel, Brain, Encoder — isolates
input polling from both. Sentinel runs at 1000Hz and is never blocked by physics or GPU.

Four dedicated threads (separate physics thread) was considered and rejected. Jolt's internal job
system already parallelises physics work across the worker pool. A dedicated physics thread would
steal a core that the worker pool uses more effectively. Brain acts as a physics coordinator instead:
it submits Jolt jobs and then steals from the physics queue while waiting.

---

### Brain and Encoder as Coordinators, Not Workers

On an 8-core machine: 1 Sentinel + 1 Brain + 1 Encoder = 5 workers remaining. If Brain and Encoder
were pure coordinators (blocked waiting for jobs to finish), that's 5 effective cores for logic and
render. By acting as workers themselves while waiting, you get 6 effective logic cores and 6 effective
render cores (workers steal from both queues). ~20% throughput gain for free — no extra threads, no
extra synchronisation.

---

### SoA Field Arrays + FieldProxy OOP Syntax

Struct of Arrays is the right layout for SIMD batch processing — all PosX values contiguous, all PosY
values contiguous, cache-line utilisation near 100% during sweeps. The problem: SoA is hostile to
component authors. Writing `posXArray[i] += velXArray[i] * dt` for every field is error-prone and
leaks the data layout into gameplay code.

`FieldProxy<T, WIDTH>` wraps raw array pointers behind operator overloads so that
`transform.PosX += velocity.VelX * dt` compiles to a direct SoA array access. Gameplay authors
write OOP-style code; the engine layout is completely opaque to them. No virtual dispatch, no map
lookups — `operator T()` and `operator=` are direct pointer dereferences.

The three widths (Scalar / Wide / WideMask) let the same component type participate in both
scalar Construct ticks and 8-wide AVX2 entity sweeps without any code change at the component level.

---

### Tiered Storage — Why Four Tiers, Not One

A single SoA ring buffer for everything would waste memory on entities that never roll back and waste
bandwidth on entities that never render. The four tiers exist because the set of things you need to do
with data is not uniform:

| Tier     | Ring depth | Why                                                                           |
|----------|-----------|--------------------------------------------------------------------------------|
| Cold     | 0          | Rarely updated config data. AoS in chunks. No iteration cost.                 |
| Static   | 0          | Read-only geometry. Separate array. Never touched by update loops.            |
| Volatile | 3          | Triple-buffer for Logic↔Render handoff. No rollback needed.                   |
| Temporal | N          | Rollback history. Only entities that matter for netcode pay the memory cost.  |

The tier is declared on the **component**, not the entity. An entity's effective tier is the highest
tier of any of its components. This means a single macro change at the component level can promote
an entire class of entities to rollback-capable without touching entity or system code.

When `TNX_ENABLE_ROLLBACK` is off, Temporal is treated as Volatile. Games that don't need rollback
pay zero memory cost for the tier system.

---

### Volatile = 3 Frames (Not More)

Originally 5 frames were used to give the render thread comfortable headroom. After moving to
GPU-driven rendering with a persistent previous-frame InstanceBuffer on the GPU side, the render
thread only needs to supply frame T — the GPU interpolates T-1 from its own buffer. The CPU slab
needs only 1 frame for logic and 1 for render to work on simultaneously, so 3 (triple-buffer) is the
correct minimum. 5 frames wasted memory with no benefit.

---

### Dual-Ended Arena Partition Layout

Physics must iterate players + AI + physics props densely. Rendering must iterate particles + decals
+ players densely. These sets overlap (players appear in both) but are not identical. A single
contiguous array for everything works but forces both systems to skip entities that don't apply to
them — gap-skipping adds branches and breaks prefetch.

The dual-ended arena solves this with zero padding overhead:

```
Arena 1:  [RENDER →] .... [← DUAL]     (renderable)
Arena 2:  [PHYS →]   .... [← LOGIC]   (cached/physics-only)
```

Physics iterates DUAL + PHYS contiguously across the arena boundary — a dense wall with no gap.
Render iterates RENDER + DUAL with one gap in Arena 1 — handled by the GPU predicate pass at
negligible cost. The group is auto-derived from component `SystemGroup` tags. There is no manual
annotation; that would be a footgun that silently puts entities in the wrong partition.

---

### Why Jolt Owns Physics State

The conventional pattern is: ECS pushes transforms to physics, physics steps, ECS pulls results.
This requires pushing every entity's state every frame even when nothing changed.

Jolt owns physics state here. The ECS only writes to Jolt on explicit overrides (spawn, teleport,
impulse, kinematic target). Jolt's integrator advances bodies internally. After each step, only
**awake** bodies are pulled back into the SoA write arrays. A scene with 50K bodies where 200 are
moving pays for 200 pulls — not 50,000.

The tradeoff: velocities live in Jolt, not the ECS. Gameplay code that needs velocity queries Jolt
directly during ScalarUpdate. This is acceptable — velocity reads are rare and scalar.

---

### Physics Divisor (512Hz logic / 64Hz physics)

Running Jolt at 512Hz would be prohibitively expensive for any meaningful scene. The 8:1 divisor
(default: 64Hz physics) is configurable — games that need tighter physics can set it lower.

For rollback: when rolling back to frame N, the engine snaps to the nearest Jolt execution frame
at or before N (e.g., rollback to frame 100 → restore from frame 96 at 8:1), restores Jolt state,
and resimulates from there. At most 7 frames of physics approximation in the worst case — acceptable
for competitive multiplayer. Rebuild-from-slab (positions only) was tested and rejected: it restarts
the solver cold, causing determinism divergence in the contact cache and warmstarting state.

---

### Constructs vs Entities — OOP Over Data-Oriented Substrate

The engine has two gameplay object types because two fundamentally different things exist in a game:

- **The horde** — zombies, bullets, particles. Homogeneous. High count. No bespoke logic. Swept by
  SIMD. Represented as raw ECS data (Entities).
- **The thinkers** — Player, GameMode, AIDirector. Singular. Complex. Bespoke logic. Represented as
  `Construct<T>` (CRTP OOP objects).

Forcing the horde into Constructs would make SIMD batch processing impossible. Forcing the thinkers
into ECS entities would make per-object logic unnatural and kill the OOP authoring experience.

The split lets entity authors write `transform.PosX += velocity.VelX * dt` (which the engine sweeps
8-wide) while Construct authors write `if (IsGrounded()) Jump()` (which runs scalar). Both are
correct for their use case. Neither pays the cost of the other.

---

### CRTP vs Virtual for Constructs

`Construct<T>` uses CRTP. Auto-registration of tick hooks (`PrePhysics`, `PostPhysics`,
`PhysicsStep`, `ScalarUpdate`) is done via `if constexpr` concept detection at compile time.
If you implement the method, you get the tick. If you don't, you pay nothing — no vtable slot,
no virtual dispatch overhead, no base-class stub.

Virtual functions were explicitly ruled out for components (SchemaValidation enforces this) because
vtables break SoA decomposition — a vtable pointer in a component struct would pollute every SoA
array slot. Constructs are scalar objects so virtual dispatch is technically fine there, but CRTP
was chosen anyway to keep the pattern consistent and to enable compile-time interface contracts via
C++20 concepts on `Owned<T>`.

---

### Owned<T> for Construct Composition

Complex Constructs compose via `Owned<T>` value members rather than heap allocation or inheritance.
This gives:
- **Deterministic init/destroy order** — members initialise in declaration order, destroy in reverse
- **Zero allocation** — owned objects live inline, no heap indirection
- **Compile-time interface contracts** — `Owned<T>` can require C++20 concepts (`Targetable`,
  `Damageable`) giving zero-cost compile-time replacement for runtime gameplay tag queries
- **Each owned Construct is exactly as heavy as it needs to be** — `TargetingSystem` without
  a `ConstructView` pays no ECS cost; `AmmoFeed` without a tick method pays no tick cost

---

### Soul / Body Split

`Soul` (session identity) and `Body` (world presence) are separate because they have different
lifetimes. A Soul survives level transitions — it is the player, not the player's character. A Body
is destroyed when the World resets and recreated by the GameMode when the player re-enters gameplay.

This also cleanly handles spectators (Soul exists, no Body), disconnected players (Soul in grace
period, Body released), and late joiners (Soul created at handshake, Body created when GameMode
decides spawn conditions are met).

The Soul/Body pattern is designed to work identically in standalone. Souls are always created —
in networked sessions by the handshake path, in standalone synthesised directly by FlowManager.
This unifies the flow graph across all play modes and enables local co-op (two Souls, two
InputBuffers, two Bodies, one machine) without a separate code path.
*Note: standalone Soul synthesis is not yet implemented — see Known Issues #6.*

---

### InputLead — Tunable Prediction Window

`MaxClientInputLead` (currently 64 frames, ~125ms at 512Hz) is a fixed test value, not a derived
constant. The correct value is RTT-dependent and game-dependent: a twitch shooter needs less lead
than a strategy game. The intent is to make the injector extensible — `InjectInput` is designed as
a swap point where different prediction strategies can be plugged in per-game (carry-forward,
interpolation, ML-predicted, game-specific movement extrapolation). The lead window controls how far
ahead the server is willing to simulate without hearing from a client before stalling.

---

### OwnerID Bit Width (8 bits / 256 max)

8-bit OwnerID is a pragmatic starting point — more than enough for any foreseeable use case. The
intent is that handle and header bit widths are eventually user-configurable (smaller for bandwidth-
constrained games, larger for MMO-scale). Designing that flexibility up front correctly is a
significant undertaking and is deferred to a dedicated hardening pass. The `static_assert` on
`EntityRef` will catch any mismatch before silent breakage if the width ever changes.

---

### Three Travel Levers, Not a Single Policy

Most engines expose "seamless travel" vs "hard travel" as a binary choice. This conflates three
orthogonal concerns: what happens to the World, what happens to Constructs, and what happens to the
network session. Different games need different combinations — a battle royale resets everything
between matches; a persistent world keeps the session and swaps only the level.

Trinyx exposes three independent levers:
- **Domain lifetime** — Keep World + swap Level | Reset World | Keep nothing
- **Construct lifetime** — Persistent (survives via reinitialization) | World-scoped | Level-scoped
- **Network continuity** — Keep NetSession | Swap NetSession

Games compose these to build their flow. The engine enforces the contracts; the game owns the
decision. A `FlowState` declares what it requires; `FlowManager` creates and destroys accordingly.

---

### NetChannel as the Replication Boundary

`NetChannel` wraps a `ConnectionInfo*` and `ISteamNetworkingSockets*` — two pointers — into a typed
send API. The intent is that it becomes the natural home for all per-connection state over time:
delta compression baselines, message coalescing buffers, reliability policy tables, RPC dispatch.

Keeping this behind `NetChannel` means the transport (GNS today, potentially something else later)
is entirely behind an implementation boundary. The gameplay layer calls `channel.Send<T>(type, payload)`
and never sees a socket handle.

---

### 30Hz Net Tick — A Starting Point, Already Evolved

The 30Hz default is not a design ceiling — it is a conservative starting point. The actual
architecture has already diverged from a single net tick rate:
- **Input frames** are sent as fast as input is produced (not gated to 30Hz)
- **ACK heartbeats** run on the replication tick but are decoupled from state corrections
- **State corrections** run on the replication tick

The logical end state is per-message-type rate control: input at input-polling rate, corrections at
network tick, RPCs on-demand. The 30Hz default will be revisited once delta compression reduces the
per-correction payload size enough to justify higher correction rates.

---

### 5 GPU InstanceBuffers — Breaking the VSync Chain

Without multiple InstanceBuffers: VSync holds the GPU buffer → render thread blocks waiting for it →
render thread holds the slab read lock → logic thread stalls waiting for the lock. One stall cascades
all the way to simulation.

5 InstanceBuffers (cycling independently of the 2 in-flight GPU frame slots) ensures the render
thread always has a free buffer to write into. Logic and render are fully decoupled. If the render
thread falls behind by more than 5 frames it becomes a renderer performance problem — not a
synchronisation problem that contaminates the simulation.

---

### Cumulative Dirty Bit Array — SIMD OR Path

When render falls behind multiple logic frames, it cannot simply upload the most recent frame's data
— entities modified in intermediate frames would be missed. The solution: a double-buffered cumulative
dirty bit array (12.5KB for 100K entities — fits in L1). Logic ORs dirty bits atomically per field
write. If render misses N frames, it SIMDs OR the dirty arrays from all N intermediate frames before
building the upload set. No full-slab copy required unless lag exceeds the ring buffer depth.

The same bit 30 drives GPU upload and rollback blast radius. The dirty set from a rollback correction
propagates naturally through update logic to exactly the entities that need recomputation — no manual
dependency tracking required.

---

### GPU Interpolation is Self-Contained

The render thread uploads frame T only. The GPU keeps frame T-1 in its own persistent previous-frame
InstanceBuffer. The scatter shader lerps between them at the current sub-frame alpha. The CPU slab
never needs to supply two frames simultaneously — this is what allows the Volatile tier to be only
3 frames deep rather than 5.

---

### Replay & Recording — Architecture Wins

All deterministic simulation state lives in contiguous, trivially-copyable SoA field arrays. Replay
recording is serialization in `PropagateFrame` — no new infrastructure. SoA arrays compress far
better than AoS: homogeneous float arrays (all PosX, all PosY) have high spatial coherence, and
delta compression between frames yields mostly zeros since most entities are near-stationary per tick.

Free wins:
- **Kill cam / rewind** — slab ring buffer indexed backward, re-rendered with different camera target
- **Spectator scrubbing** — random seek to any snapshot, resume simulation forward
- **Anti-cheat** — diff server vs client slab timelines offline; divergence flags cheating
- **Sub-tick precision** — input events timestamped at actual ms, not frame-quantised
- **With rollback** — retroactive event insertion enables frame-perfect multiplayer replay from any
  participant's perspective

---

### Editor Rewind Falls Out for Free

The slab ring buffer that exists for rollback netcode also gives the editor timeline scrubbing with
no additional infrastructure. In editor sessions (non-deterministic build, defrag enabled), the engine
can rewind to any frame in the ring by indexing backward and re-rendering. The result is a full
frame-precise rewind for debugging — not a recorded video, but a live re-execution of the exact ECS
state at that frame. This is a direct consequence of the ring buffer existing for rollback, not a
feature that was designed independently.

---

### Singleplayer and Multiplayer Share One Code Path

FlowState transitions, GameMode lifecycle, Soul lifecycle, the GamePhase machine, and all ModeMixins
behave identically across singleplayer, local co-op, and online multiplayer. The network layer is an
additive concern. In singleplayer: `TravelNotify` becomes a local `FlowManager::Travel()` call,
`NetChannel` is absent, `PredictionLedger` is absent, and Souls are synthesised directly by
`FlowManager` instead of being triggered by a handshake. Everything else — `WithSpawnManagement`,
`WithLobby`, `WithRespawn`, `WithTeamAssignment` — compiles and runs unchanged. Game code written
for singleplayer is online multiplayer code.

---

### Local Co-op is Just Multiple Souls With No Network Layer

Local co-op extends singleplayer by creating one Soul per connected controller instead of one.
OwnerIDs are assigned locally by `FlowManager`. The GameMode sees `OnPlayerJoined` fire twice and
spawns two Bodies — exactly what it would do online. `InputLead` stores a controller index instead
of a network OwnerID. Split-screen viewport assignment is a FlowState concern, not a GameMode
concern. The GameMode is completely unaware whether its players are local or remote.

---

### Constructs Serialize Only Through Views

Construct scalar C++ members are not serialized. Only View-owned ECS data is serialized — it flows
through the existing ECS path with no special-case code. If a value needs to survive serialization
(e.g. `TurretBase::MaxAmmo`), it belongs in a component. This is not a restriction; it is a forcing
function: anything worth saving belongs in the data model, not in OOP object state.

The consequence: loading a level is just hydrating ECS data and calling `PostInitialize()`. Constructs
re-derive transient state from their Views. There is no save-game pointer fixup, no object graph
serialization, no version migration for OOP members. The ECS schema is the serialization schema.

---

### FlowState Declaration Contracts Replace Lifecycle Boilerplate

In most engines, the gameplay programmer is responsible for manually creating and destroying the World,
NetSession, and physics when transitioning between states. Forgetting a step leaks resources or
crashes.

`FlowState::GetRequirements()` declares what a state needs; `FlowManager` creates and destroys
accordingly during every transition. The programmer can't forget — they never call the create/destroy
functions. The declaration is the contract and the engine enforces it.

---

### EntityCacheIndex as a Globally Stable Coordinate

The entire engine uses a single flat `EntityCacheIndex` as the column coordinate across all tiers —
Volatile, Temporal, Cold chunk mirrors. This is the "global spreadsheet" model: any field for any
entity is at `(field_array_base + EntityCacheIndex)`, regardless of tier. No tier-switching lookup,
no secondary index, no translation table.

The consequence: defrag that moves entities must fire identity-change notifications so Views rebind.
In determinism builds, defrag of live authoritative entities is disabled to keep indices stable across
rollback windows. Slot reuse (tombstoned slots refilled) is allowed because it doesn't change
existing indices.

---

### Two Handle Counters on Archetype Prevent Iteration Undercount

`RemoveEntity` tombstones in place — it clears the Active flag but does not compact data.
`AllocatedEntityCount` (high-water mark, never decremented) drives iteration bounds.
`TotalEntityCount` (live count, decremented on removal) is for diagnostics only.

If iteration bounds used the live count, the update loop would stop short of the end of allocated
space and skip live entities that happen to sit past a run of tombstoned slots near the tail. The
bitplane scan skips tombstoned entities for free; the high-water mark guarantees no live entity is
ever missed.

---

### Fixed-Point → GPU float32 is the Only Lossy Step, and It's Acceptable

The full simulation pipeline uses Fixed32 (int32, 0.1mm precision). The only conversion to float32
happens at render-thread upload time: fixed-point cell-relative position → camera-relative float32
for the GPU. At ≤1km from the camera, float32 gives ≈0.05mm precision — finer than the 0.1mm unit
definition. The conversion is lossless in practice and happens on the render thread, outside the
authoritative simulation path. The GPU never sees the fixed-point representation.

This means the entire determinism guarantee lives on the simulation side, and the render side gets
full GPU float32 throughput with no precision concerns.

---

### Jolt Snapshot Restore vs Rebuild From Slab

When rolling back, two approaches were considered: (1) restore a saved Jolt snapshot, (2) rebuild
Jolt state from ECS positions only. Rebuild was tested and rejected. Jolt's contact cache and solver
warmstarting state cannot be reconstructed from positions alone — a cold restart diverges from the
original timeline because the solver initialises constraint forces from scratch rather than continuing
from the previous solution. The snapshots are ~7KB per physics frame (56 bodies) and fit trivially
in the ring buffer. Snapshot restore is the only correct approach for deterministic resim.

---

### PIE Loopback — One Connection Manager, Two ConnectionInfo Legs

In PIE, a single `NetConnectionManager` sees both ends of every loopback connection. For OwnerID 1,
there are two `ConnectionInfo` entries: one `bServerSide=true` (the server-accepted handle) and one
`bClientInitiated=true` (the client-opened handle). `HandleMessage` in `PIENetThread` routes by
`bServerSide` — server messages go to `ServerNetThread::HandleMessage`, client messages go to
`ClientNetThread::HandleMessage`. This means the full server + client handshake, input routing, ACK
pipeline, and replication all run in the same process with real message dispatch — PIE is not a
simulation of networking, it is networking over a loopback socket. Bugs found in PIE are real bugs.

`FindConnectionByOwnerID` must be called with `requireServerSide=true` when the server side needs
to set state on its own leg, or it will silently mutate the client leg (which shares the OwnerID).

---

### SoA Layout Is Already the GPU Layout

The temporal component cache is already SoA — all PosX values contiguous, all PosY values contiguous.
The GPU upload path uploads raw slab slices directly into SSBOs via Buffer Device Address. There is no
AoS→SoA conversion on the upload path; the data is already in the layout the GPU wants. The compute
shaders (predicate → prefix_sum → scatter) then compact and interpolate on the GPU. The CPU render
thread dispatches three compute passes and does no entity-wise work. This layout decision — made for
SIMD batch processing — turns out to be exactly correct for GPU-driven rendering as well.

---
