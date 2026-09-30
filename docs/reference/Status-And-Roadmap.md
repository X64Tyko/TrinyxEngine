# Status & Roadmap

> [Home](../Home.md) | [Design Decisions →](Design-Decisions.md) | [Known Issues →](Known-Issues.md)

---

## Timeline

**Project start:** ~2026-02-01  
**Last updated:** 2026-09-29  
**Phase:** Foundation Stage — animation and networking in progress

---

## Stage 1: Foundation

| # | Milestone | Status | Notes |
|---|---|---|---|
| 1 | **Editor (bare-bones)** | ✅ Complete | 8 panels + Construct / Entity / Prefab editor windows, Construct/Entity/Component generators, ImGuizmo, PIE (local single-player world + networked 1–4 clients), scene snapshot/restore, asset database (.tnxid sidecars, .tnxdb), JSON .tnxscene, 50-command undo/redo, GPU picking. Single main window (ImGui multi-viewport disabled). |
| 2 | **Construct/View OOP** | ✅ Complete | `Construct<T>`, `Owned<T>`, `ConstructView<TEntity>`, `ConstructBatch`, JoltCharacter. PlayerConstruct proven. |
| 3 | **Networking** | In Progress | GNS wrapper, PIE loopback, entity replication, clock sync, input routing, delta compression. `LogicThread<TNet,TRollback,TFrame>`, `ServerClientChannel`, `AuthoritySim`/`OwnerSim` done. Two network modes (Deterministic/Non-Deterministic), `ListenNet` mode, host migration, disconnect policy, and mode-split rewrite designed and planned. |
| 4 | **Audio** | ✅ Complete | SDL3 `AudioManager`: voice pool, handle-based playback, event registry, per-voice fade, priority voice stealing. Anti-Event compatible. |
| 5 | **Camera System** | ✅ Complete | `CameraManager` (per-Soul layer stack), `CameraSlot[5]`, `CameraLayer` + mixins, `ECameraNode` (cold), `ECamera` (hot SoA), `CurveHandle`. |
| 6 | **Game Flow** | In Progress | FlowManager, FlowState, GameMode, Soul, NetChannel done. `WithSpawnManagement`, `WithLobby`, `WithTeamAssignment` ModeMixins done. `ClientRepState` 7-state machine done. |
| 7 | **Animation** | In Progress | Rollback-safe, replicated animation state: `CAnimBase` (clip or GPU-evaluated blendspace, cross-fade, root-motion flag, state-machine node for rollback re-derivation) and `CAnimLayer` (2 overlay slots, bone masks, replace or additive) are Temporal components. `AnimConstruct` mixin: state machine, root motion, notifies, sockets, FK bone cache. GPU compute skinning with indirect dispatch. glTF skeleton/animation import. Replicated animated character proven on clients. Pending: IK, retargeting, animation graph tooling. |

---

## Stage 2: Hardening

Once Editor + Networking + Audio are stable and a test arena level is running, the engine enters a dedicated cleanup and rewrite phase.

**Hardening targets:**

- Hot-path data structure audit for cache efficiency
- Archetype field allocation and meta storage cleanup
- ConstraintEntity system (constraint pool, rigid attachment pass, physics root determination)
- Static entity tier (requires asset importing)
- Reflection system robustness (static init ordering — currently fragile across TUs)
- `TNX_STRIP_NAMES` build option for shipping builds (strip registration name strings)
- Default identity quaternion for `CTransform` (`RotQW=1.0f`) to prevent zero-quaternion rendering bugs

**After hardening:** Arena shooter test level to prove the full stack — high entity counts, physics, competitive input latency, rollback netcode, networked multiplayer.

---

## Completed Work (Selected)

### Architecture

- [x] Three-thread architecture (Sentinel / Brain / Encoder) — fully operational
- [x] Raw Vulkan stack (volk 1.4.304 + VMA 3.3.0, `vk::raii::`) replacing SDL3 GPU backend
- [x] GPU-driven compute pipeline (predicate → prefix_sum → scatter, Slang shaders, BDA)
- [x] Dirty-bit-driven selective GPU upload — only modified entities uploaded per frame (2026-05)
- [x] Lock-free job system (MPMC Vyukov ring buffers, futex-based wake, core-aware pinning)
- [x] Tiered storage partition layout (Cold/Static/Volatile/Temporal, dual-ended arena)
- [x] 5 GPU InstanceBuffers (cycling independently of 2 GPU frame-in-flight slots)

### ECS

- [x] `FieldProxy` (Scalar / Wide / WideMask, `FieldProxyMask` zero-size base)
- [x] `TemporalComponentCache` SoA ring buffer
- [x] `TemporalFlagBits` typed enum (Active, Dirty, DirtiedFrame, Replicated, Alive, Tombstone, …)
- [x] `TNX_TEMPORAL_FIELDS` / `TNX_VOLATILE_FIELDS` with SystemGroup tag (auto-derives partition)
- [x] `SchemaValidation.h` (no vtable + all fields must be FieldProxy — compile-time)

### Physics

- [x] Jolt Physics v5.5.0 (`JoltJobSystemAdapter`, `CJoltBody` volatile, slab-direct iteration)
- [x] `JoltCharacter` — `CharacterVirtual` wrapper for Construct-driven character controllers
- [x] Rollback netcode — `SaveState`/`RestoreState` snapshot ring buffer, `ExecuteRollbackTest`, byte-perfect determinism verified (2026-03-29)

### Networking

- [x] `LogicThread<TNet, TRollback, TFrame>` three-axis policy template (2026-05)
- [x] `AuthoritySim` / `OwnerSim` / `SoloSim` net policy types
- [x] `ServerClientChannel` — per-client: `PlayerInputLog`, `Replicated[]`, `PendingActivations`, `NetChannel`, `PendingPacketQueue`
- [x] `ClientRepState` 7-state machine (PendingHandshake→Synchronizing→Loading→LevelLoading→LevelLoaded→Loaded→Playing)
- [x] Delta compression — `EntityDelta` (component-level dirty patches), `InputFrameDelta` (delta-encoded input window)
- [x] Entity destruction replication (`EntityDestroy` / `ConstructDestroy` wire types)
- [x] Entity activation pipeline (`EntityActivate`, `StreamLoad`/`StreamReady`/`ChunkActivate`)
- [x] `PredictionLedger` — client-side in-flight spawn prediction tracker
- [x] Construct replication consistent with entities for late join and rejoin
- [x] Finer-grained level streaming control (what loads where, when, and how it activates)
- [x] First hook for non-lockstep networking when determinism is disabled

### Animation

- [x] `CAnimBase` / `CAnimLayer` Temporal animation components (rollback + replication)
- [x] Blendspace evaluation on the GPU, cross-fades, masked replace/additive overlay layers
- [x] `AnimConstruct` state machine mixin — root motion, notifies, sockets, FK bone cache
- [x] GPU compute skinning (`SkinningPass`, indirect dispatch)
- [x] glTF skeleton and animation import

### Rendering

- [x] Per-mesh indirect draws — `build_draws` + `sort_instances` passes after scatter

### Tooling

- [x] Rollback determinism test harness with watchdog thread (CI-safe stall detection)
- [x] Formatting, linting, and presubmit tooling (clang-format, clang-tidy, pre-commit hooks, presubmit build + Testbed)

### Math

- [x] `FixedUnit` — unit-range fixed-point for trig output and direction components
- [x] `Fixed32` (int32, 0.1mm precision, all arithmetic ops, `FixedSqrt`)
- [x] `SimFloat` alias — `SimFloatImpl<float>` or `<Fixed32>` via `TNX_DETERMINISM`
- [x] `FixedTrig` (`FixedSin`/`FixedCos` LUT)
- [x] Jolt fixed-point bridge validated — engine runs deterministically

### Game Flow

- [x] `FlowManager` — state stack, travel primitives, World/Level/Mode lifetime management
- [x] `FlowState` base class with `StateRequirements` declaration hook
- [x] `GameMode` base class + `Construct<T>` opt-in for ticks
- [x] `Soul` (OwnerID identity, `ClaimBody`/`ReleaseBody`, RPC dispatch)
- [x] `NetChannel` typed per-connection send wrapper
- [x] `ModeMixin` system — `WithSpawnManagement`, `WithLobby`, `WithTeamAssignment`
- [x] Travel toolbox — three orthogonal levers (domain lifetime, Construct lifetime, network continuity)
- [x] `GameModeManifest` / `ClientModeManifest` CRTP typed payloads

---

## Not Yet Implemented

### Networking

- [ ] **`ListenNet` compile-time mode** — `ListenSim` TNet policy; `AuthorityClass` tags (`Host`/`Owner`/`Fixed`) for per-entity authority ownership in listen-server scenarios; runtime authority override table for resolution
- [ ] **Host migration (ListenNet)** — handshake protocol, candidate election (ranked by latency + snapshot recency), authority transfer with zero peer gap; prerequisite: snapshot serialization path
- [ ] **Snapshot serialization path** — serialize/deserialize full world state into snapshot format; feeds host migration, save states, late join, and debug replay
- [ ] **Disconnect policy** — `ClientHealthMetrics`, `ClientAction`, `NetDisconnectPolicy` structs; `ClientHealthCallback` for game-owned policy; default threshold enforcement wired to NetThread health checks
- [ ] **Deterministic / Non-Deterministic mode split** — separate `AuthoritySim`/`OwnerSim` paths for non-deterministic mode; uses server timestamps (`server_time_us`) instead of frame numbers, state replication as primary sync signal, variable client Hz, 30Hz snapshot interpolation
- [ ] **Networking without rollback** — owner-only prediction and replay, per-client missing-input policy (no global stall outside lockstep), continuous clock sync with client time dilation, discrete input event injection, `PlayerInputLog` synchronization. See [Networking Without Rollback](../networking/Overview.md#networking-without-rollback--current-state-and-direction).
- [ ] **Server-only rollback** — Authority re-simulates from its input log while Owners run without rollback (build flags already independent; PIE's server already does this — shipped server/client split pending)
- [ ] **Lag compensation** — server-side hit-validation history
- [ ] **Remote entity representation** — open design ("shooting a shadow of the target")
- [ ] **Phase 0 tentative despawn** — per-frame `TentativeDestroys` ring buffer for rollback-safe entity death (Phase 1–3 done; Phase 0 tracking not yet implemented)

### Simulation

- [ ] **Trinyx physics solver** (long-term) — solver state in slab fields; prerequisite for full client prediction of physics-driven entities
- [ ] **ConstraintEntity system** — constraint pool, `ConstraintType` enum, render-thread rigid attachment pass, physics root determination
- [ ] **Space partition cell registry** — cell world origins, cell assignment at spawn, cross-cell reparenting
- [ ] **Standalone Soul synthesis** — create a local Soul in `FlowManager` for offline/solo play (currently Soul creation is gated behind network handshake)

### Presentation

- [ ] **Presentation Reconciler** — Anti-Events (RapidFadeOut, SoftCancel, RapidDecay) for rollback-driven effect correction. `AudioManager::FadeOut` is Anti-Event-compatible; diff logic not implemented.

### Rendering

- [ ] **Frustum culling** — SIMD 6-plane test + GPU-side predicate enhancement
- [ ] **Texture pipeline, material model, shadows** — not yet designed; prerequisites for the VizBuffer material resolve
- [ ] **State-sorted rendering** — 64-bit sort keys, GPU radix sort after scatter
