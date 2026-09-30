# Known Issues

> [← Design Decisions](Design-Decisions.md) | [Schema Errors →](Schema-Error-Reference.md) | [Home](../Home.md)

---

## Networking

### 1. `Soul::Channel` is a dead stored member

`Soul` holds a persistent `NetChannel` member, but `DispatchServerRPC` / `DispatchClientRPC` unconditionally overwrite it from `RPCContext` on every call. The stored value is never read between dispatches.

**Correct fix:** Thread `NetChannel` through the dispatch call and have RPC thunks capture it from dispatch context. Eliminates `GetNetChannel()` and the stored member entirely.

---

### 2. `NetChannel` CI pointer can dangle in PIE

`NetChannel` stores a raw `ConnectionInfo*`. If `NetConnectionManager::Connections` (a `std::vector`) reallocates on a new connection, all outstanding `NetChannel` objects hold dangling pointers.

**Correct fix:** Stable storage for `ConnectionInfo` (e.g. index-based or pointer-stable pool), or `NetChannel` stores an index + generation instead of a raw pointer.

---

### 3. `FindConnectionByOwnerID` was ambiguous in PIE

In PIE, two `ConnectionInfo` entries share the same `OwnerID` — one `bServerSide=true`, one `bClientInitiated=true`. The function now accepts a `requireServerSide` flag to disambiguate.

**Audit required:** All call sites must pass the correct flag. Any new call site added without the flag will silently mutate the wrong leg.

---

### 4. `SendPong` builds its header manually, bypassing `MakeHeader`

Every other send path goes through `NetChannel::MakeHeader` which stamps `LastAckedClientFrame`. `SendPong` was constructing the header by hand and omitting this field — fixed — but the pattern is a regression risk.

**Correct fix:** Make `SendPong` call `MakeHeader` like all other sends.

---

### 5. Heartbeat Ping reuses the clock-sync message type

`TickReplication` sends a `NetMessageType::Ping` to propagate ACKs during quiet frames. This conflates ACK heartbeats with RTT measurement pings.

**Correct fix:** A dedicated `NetMessageType::Ack` (header-only, no clock-sync semantics) would be cleaner.

---

### 6. Souls do not exist in standalone mode *(blocking for local play)*

The Soul creation path is gated behind the networking handshake. In standalone, no Souls are created — gameplay code that queries Souls finds nothing.

**Correct design:** Always create Souls — synthesise a local Soul per player during standalone World init, `OwnerID` from a local counter, no net session. This unifies the code path and enables local multiplayer without a divergent flow graph. `Soul::GetNetChannel()` must be null-safe when no CI exists.

**Status:** Not yet implemented. See [Status & Roadmap](Status-And-Roadmap.md).

---

### 7. `PlayerInputLog::Store()` high-water guard is belt-and-suspenders

`HighWaterFirstFrame` was stuck at 1 for the entire session because ACK trimming was broken. The `Store()` loop is now clamped to `LastConsumedFrame - Depth + 1` regardless of ACK state, bounding it to `O(ring_depth)`. Once ACK trimming is verified stable in production, the belt-and-suspenders clamp can be removed if desired.

---

### 16. Networking without rollback has no reconciliation for the controlled entity

With rollback compiled out, corrections are written straight into the current frame and unacknowledged inputs are not
replayed — rubber-banding proportional to latency. See
[Networking Without Rollback](../networking/Overview.md#networking-without-rollback--current-state-and-direction) #1.

---

### 17. One lagging client stalls the whole server

`AuthoritySim` stalls the sim for everyone when any player exceeds `MaxClientInputLead` (default 16 frames), in every
build mode. Correct for lockstep only. See [Networking Without Rollback](../networking/Overview.md#networking-without-rollback--current-state-and-direction) #2.

---

### 18. Late input is ignored when the server runs without rollback

Late input lands in the server's `PlayerInputLog` but only rollback acts on it. **Correct fix:** server-only rollback
(the build flags are already independent; the server/client split needs wiring). See
[Rollback Netcode](../networking/Rollback-Netcode.md#server-only-rollback-planned).

---

### 19. Clock sync runs once; no client time dilation

`FrameOffset` and `InputLead` are set at the handshake only. Clients that can't sustain 512Hz or that drift fall behind
with no correction. See [Networking Without Rollback](../networking/Overview.md#networking-without-rollback--current-state-and-direction) #4.

Observed in PIE: one Owner can settle a frame short of the lead budget (`lastReceived = frame - 17`, lead 16), so the
Authority stalls on that Owner about once per input packet for the whole session, adding jitter to every Echo.

---

### 20. No lag compensation

No server-side rewind for hit validation. Tied to the open remote-entity-representation design. See
[Networking Without Rollback](../networking/Overview.md#networking-without-rollback--current-state-and-direction) #5–6.

---

### 21. `PlayerInputLog` is unsynchronized

NetThread writes and the Logic thread reads without a lock; safe only while their phases don't overlap.
**Correct fix:** per-log spinlock or lock-free handoff.

---

### 22. Discrete input events are not injected on the server

Only held-key state is injected (`AuthorityNet.cpp` `TODO`). **Correct fix:** inject per-frame discrete events from
the input log.

---

### 23. ~~Owner and Authority could disagree on facing~~ ✅ Fixed (2026-09-29)

View yaw/pitch lived on `PlayerConstruct` and were integrated from per-frame mouse deltas: the Owner read the
presentation buffer while the Authority read the injected sim delta, resimulation re-integrated them, and extrapolated
input frames zero the mouse delta, so "forward" could drift apart permanently.

**Fix:** absolute view angles travel in sim input (`InputSnapshot::ViewYaw/ViewPitch`, `InputDeltaFlags::HasView`),
are recorded and replayed by rollback (`TemporalFrameHeader::InputViewYaw/Pitch`), and extrapolated frames repeat the
last known facing. Gameplay reads facing only from sim input; the Owner writes new angles outside resimulation. The
simulated facing is stored in the Temporal `CControlRotation` component, so it rolls back and replicates to Echoes.

---

## ECS / Memory

### 8. ~~`TemporalFrameStride` duplicated on Archetype~~ ✅ Fixed (2026-04-21)

`BuildFieldArrayTable` moved out-of-line to `Archetype.cpp`; queries `cache->GetFrameStride()` directly.

---

### 9. ~~`GetTemporalFieldWritePtr` lived on Archetype~~ ✅ Fixed (2026-04-21)

`GetWriteFramePtr(void*)` and `GetReadFramePtr(void*)` added to `ComponentCacheBase`; all call sites updated.

---

### 10. Reflection system relies on static initialisation order *(fragile)*

`TNX_REGISTER_COMPONENT`, `TNX_TEMPORAL_FIELDS`, `TNX_REGISTER_SCHEMA` etc. are driven by static constructors. Cross-TU ordering is undefined in C++. Currently works because all registrations resolve before `TrinyxEngine::Initialize()`, but this is fragile.

**Correct fix:** A dedicated precompile step (like UBT) or an explicit registration call per module. See [Determinism](../math-and-determinism/Determinism.md) for the planned `MetaRegistry` design.

---

## Editor

### 12. Editor writes the slab from the render thread

The ImGui editor runs on the Encoder thread and writes field values directly into the active write frame
(`UndoCommand.cpp` `GetFieldPtr`) while the paused Brain loop keeps publishing that frame. Two threads write the slab.

**Correct fix:** Brain as the only slab writer; the editor submits edit commands.

---

### 13. Undo commands hold raw storage pointers and full JSON snapshots

`EntityTransformCommand` and `ComponentFieldChangeCommand` store `Archetype*` / `Chunk*` / slot indices, which dangle if
defrag moves the entity or its slot is reused. `EntityTransformCommand` also serializes every entity field to JSON for a
single transform change. The stack is capped at 50 commands.

**Correct fix:** Handle-based transactions with changed-field deltas and the history ring as a fast path.

---

### 14. ImGui multi-viewport windows are driven from the render thread

Detached editor windows vanished or stopped responding because ImGui's platform windows were created and updated on the
Encoder thread while SDL events are pumped on Sentinel. Multi-viewport support was disabled (`f095d3b`).

**Correct fix:** All OS windows created and pumped on Sentinel.

---

## Rendering

### 11. Default identity quaternion not enforced on `CTransform`

A zero quaternion (`RotQW=0`) is mathematically invalid and produces degenerate rendering. Spawned entities that don't explicitly set rotation will render incorrectly.

**Correct fix:** Initialise `RotQW=1.0f` in `CTransform`'s default state, or assert in the scatter shader.

---

### 15. Interpolation blends across entity discontinuities

`scatter.slang` interpolates between the previous and current frame at the same `EntityCacheIndex` with one global
alpha. Defrag relocation, spawning into a reused slot, and teleports blend between unrelated states for one frame.

**Correct fix:** Frame snap plus a per-entity `Discontinuity` flag that persists until the render thread acknowledges
it.

---

### 25. ~~PIE clients lost the level or their Construct~~ ✅ Fixed (2026-09-30)

A replicated spawn keyed its replay at a frame its rollback never reached (clamped to the level floor, or skipped
beyond the ring), and applied-now activations were keyed at the Authority's stamp frame instead of the frame they
were written into. Whichever client rolled back across them lost the effect. See
[Server Events](../networking/Rollback-Netcode.md#server-events--replaying-discrete-changes).

---

### 26. ~~Multiple physics worlds shared Jolt's layer filters~~ ✅ Fixed (2026-09-30)

Layer-filter tables and Jolt's global registration were file-static and torn down by the first world to shut down,
crashing the remaining PIE worlds on Stop. Tables are now per `JoltPhysics`; global registration is process-lifetime.

---

### 27. Rollback resim diverges Jolt state when the window ends on a physics step

`Rollback_Determinism` fails with `Jolt physics: DIVERGED` (first difference at byte 30) whenever the self-test fires
at `FrameNumber % PhysicsDivizor == 0`, i.e. a 9-frame resim whose last frame is a physics step boundary. ECS fields
still match. Reproduces 100% on the pre-2026-09-30 code too, by adding
`&& (logic.FrameNumber % logic.PhysicsDivizor) == 0` to the self-test trigger in `RollbackSim::ProcessRollback`.
Other phases match, which is why the test passes most runs. Suspect an off-by-one between the ground-truth Jolt
capture and the last resimulated physics step.

---

### 28. `ServerClientChannel::LastAckedSimFrame` is never updated

`DispatchDeltaCorrectionJobs` diffs each client against `LastAckedSimFrame % ringSize`, but nothing writes the field,
so every delta is computed against ring slot 0 (an arbitrary recent frame). Deltas carry absolute values for changed
fields, so state still converges, but a field that changed since the client's real baseline and happens to equal the
slot-0 value is omitted until the next heartbeat correction.

**Correct fix:** advance it from the client's acknowledged frame (the same ack that advances `LastAckedClientFrame`).

---

### 24. ~~Yaw sign differed between the rendered view and movement bases~~ ✅ Fixed (2026-09-30)

`QuatFromYawPitch` rotated by +yaw about +Y, rendering forward as `(-sin yaw, 0, -cos yaw)`, while mouse input,
`PlayerConstruct` movement, the third-person offset, the Brain free-fly, and `EditorCamera` all treat positive yaw as a
right turn with forward `(sin yaw, 0, -cos yaw)`. Mouse look rendered inverted and "forward" pointed off-view.
`QuatFromYawPitch` now rotates by -yaw, so the rendered basis is the movement basis.

---

## Planned Fixes (Next Phase)

- [ ] **Replication reliability** — client entities appear then vanish; root cause in the `ClientRepState` / activation pipeline. Partially addressed: `fb84783` fixed clients not seeing their own entity (dirty marking); the appear-then-vanish case needs re-verification.
- [ ] **Animation** — IK, retargeting, and animation graph tooling (core animation is implemented; see [Status & Roadmap](Status-And-Roadmap.md))
- [ ] **Phase 0 tentative despawn** — per-frame `TentativeDestroys` ring buffer for rollback-safe entity death
- [ ] **Frustum culling** — SIMD 6-plane test, GPU-side predicate enhancement
- [ ] **State-sorted rendering** — 64-bit sort keys, GPU radix sort after scatter
- [ ] **Standalone Soul synthesis** — synthesise a local Soul per player in `FlowManager` for offline play (Known Issue #6)
