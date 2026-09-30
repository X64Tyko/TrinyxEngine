# Rollback Netcode

> [← Entity Replication](Entity-Replication.md) | [Despawn Protocol →](Despawn-Protocol.md) | [Home](../Home.md)

---

## Overview

Rollback is gated behind `TNX_ENABLE_ROLLBACK`. When disabled, the Temporal tier is treated as Volatile and there is zero memory cost for rollback infrastructure. When enabled, the engine maintains byte-perfect deterministic resimulation: ECS slab rewind + Jolt snapshot restore + per-frame correction injection.

**Determinism status (2026-03-29):** Byte-perfect across 5–12 frame rollbacks with 100K entities + 56 physics bodies.

---

## Two State Histories

Rollback requires two independent state histories:

### 1. ECS Slab Ring Buffer

The `TemporalComponentCache` ring buffer stores N frames of SoA data per Temporal-tier component. Frame N is at `base + (frame % RingDepth) * FrameStride`. Rewinding the slab to frame N means switching the read index — no copy needed, the data is already there.

Ring depth is the max of 8 and the configured rollback depth. The minimum 8 covers the physics divisor: rolling back to frame 100 at 8:1 physics ratio means at most 7 frames of physics approximation between the nearest Jolt snapshot (frame 96) and the rollback target.

### 2. Jolt Physics Snapshots

Jolt uses `SaveState` / `RestoreState` via `StateRecorderImpl`. Per-frame snapshots are ~7KB for 56 bodies. They are stored in a ring buffer after each `PullActiveTransforms`.

**Why snapshot restore, not rebuild from slab:** Restoring only entity positions loses the contact cache, solver warmstarting, and sleep states. A cold solver restart diverges from the original timeline because constraint forces are re-initialized from scratch rather than continuing from the previous solution. Snapshot restore is the only correct approach for deterministic resimulation.

---

## Rollback Trigger

Two paths trigger rollback:

**Authority-side (`AuthoritySim`):**
`AuthoritySim::OnSimInput` calls `logic.RequestRollback(frame)` when it detects an input mismatch — an Owner's inputs for a frame arrived after the Authority already simulated that frame.

**Owner-side (`OwnerNet`):**
`OwnerNet::HandleStateCorrections` enqueues `EntityTransformCorrection` objects (with `ResimFrameDelta` for server-annotated resim roots) into `world->EnqueueCorrections`. These feed `RollbackSim::IncomingCorrections`.

---

## `RollbackSim::ProcessRollback`

```
1. Drain IncomingCorrections
2. Find earliest correction frame across all queued corrections
3. ExecuteRollback(earliestFrame):
   a. Snap to nearest Jolt execution frame at or before earliestFrame
      (e.g., target=100, physics_divisor=8 → snap to frame 96)
   b. RestoreState(jolt_snapshot[snapFrame])
   c. Rewind ECS slab: set read index to snapFrame
   d. For each frame from snapFrame to current:
      - Inject per-frame corrections (CheckAndCorrectEntityTransform)
      - Resimulate one fixed step
   e. Resume from current frame
```

**At most 7 frames of physics approximation** in the worst case (rollback target sits between two Jolt frames at 8:1 ratio). This is acceptable for competitive multiplayer.

---

## Server Events — Replaying Discrete Changes

Spawns, activations and Alive→Active sweeps are not inputs, so the resim cannot regenerate them. Each is recorded in
the `Registry` event log and replayed when the resim passes its frame. One rule keeps them from being lost:
**an event is keyed at the frame its effect is written into.**

- **Deferred effects** (replicated `EntitySpawn`) write nothing now. They are pushed unapplied at their spawn frame and
  request a rollback there; the resim writes them into the ring.
- **Applied-now effects** (ServerReady sweep, `EntityActivate`, streamed-chunk activation, level reinit snapshots) are
  already in the current write frame, so they are keyed at that frame (`PushAppliedServerEvent`).
- **Unreachable frames.** A rollback can land later than an unapplied event's frame: level-load floor, oldest Jolt
  snapshot, ring depth. At the rewind point, `ApplyPendingServerEventsBefore` applies such events, late but never lost.
  Pruning never drops an unapplied event.
- **Order at the rewind point.** The old timeline's `DirtiedFrame` bits are cleared *before* events replay, because
  the resim propagates only entities marked in the new timeline.

---

## `StateCorrectionEntry` — Resim Annotation

```cpp
struct StateCorrectionEntry {
    EntityNetHandle Handle;
    Fixed32         PosX, PosY, PosZ;
    float           RotQx, RotQy, RotQz, RotQw;
    uint8_t         ResimFrameDelta;     // frames before current that were annotated as resim root
    Fixed32         ResimPosX, ResimPosY, ResimPosZ;
    float           ResimRotQx, ResimRotQy, ResimRotQz, ResimRotQw;
};
```

`ResimFrameDelta` > 0 means the Authority detected a divergence originating `ResimFrameDelta` frames ago. The Owner patches the slab at the resim root frame position (not just the current frame) before rolling forward. This ensures the rollback starts from the exact divergence point rather than from a midpoint.

---

## `ExecuteRollbackTest`

A determinism validation pass: `ExecuteRollbackTest` performs a rollback and verifies byte-perfect determinism by `memcmp`ing the ECS slab and Jolt state snapshots before and after. Used in testing to confirm that resimulation from frame N produces exactly the same state as the original simulation of frame N.

---

## Physics Divisor and Rollback

Physics runs at `logic_rate / PhysDivisor` (default 64Hz at 512Hz logic). For rollback:

| Logic frame | Nearest Jolt frame (8:1) | Approx. error |
|---|---|---|
| 100 | 96 | 4 frames |
| 103 | 96 | 7 frames |
| 104 | 104 | 0 frames |

The 7-frame maximum approximation means that after a rollback, up to 7 frames of physics diverge from what they would have been if Jolt had a per-frame snapshot. In practice, at 64Hz physics, 7 frames ≈ 109ms — acceptable because physics corrections at this timescale are visually imperceptible.

---

## Server Input Log

The Authority always keeps a rolling per-player input log (`PlayerInputLog` in each `ServerClientChannel`), independent
of whether rollback is compiled in. It holds one slot per sim frame, depth = `TemporalFrameCount`, indexed 1:1 with the
slab. Missing frames are extrapolated by repeating the last held state (discrete events are never extrapolated). Late
input is stored and triggers `RequestRollback` — which only has an effect when the server runs with rollback.

View angles are carried as **absolute** yaw/pitch in every input frame, not as mouse deltas. An extrapolated frame
therefore keeps the Owner's last known facing (its mouse delta is zeroed), and a late frame with a different facing
marks the log dirty like any other input change. Rollback records and replays the angles with the rest of the frame
input, so resimulated movement faces the same way it did the first time.

## Server-Only Rollback (Planned)

The Authority can enable rollback while Owners don't: the server re-simulates from its input log; Owners use
non-rollback prediction and replay. Server re-simulation only needs to reproduce its own results, so it doesn't
require `TNX_DETERMINISM` — and the two flags are already independent in the build. PIE already runs its server this
way (editor builds force rollback on). What's missing is a server-rollback / client-no-rollback split for shipped
server and client builds. See
[Networking Without Rollback](Overview.md#networking-without-rollback--current-state-and-direction).

---

## `CommittedFrameHorizon`

`AuthoritySim::OnFramePublished` advances `CommittedFrameHorizon` when all Owners have submitted confirmed inputs for that frame. Frames at or before the horizon are committed — they cannot be rolled back.

This gate controls the despawn protocol: the Authority cannot free a net slot until `CommittedFrameHorizon` passes the entity's death frame. See [Despawn Protocol](Despawn-Protocol.md).

---

## `LogicThread<TNet, TRollback, TFrame>`

Rollback behavior is encapsulated in the `TRollback` policy axis:

```cpp
template <typename TNet, typename TRollback, typename TFrame>
class LogicThread : public LogicThreadBase { ... };
```

When `TNX_ENABLE_ROLLBACK` is off, `TRollback = NoRollback` — the Temporal tier becomes Volatile and `ProcessRollback` is a no-op. All rollback infrastructure compiles to nothing.

---

## Speculative Presentation and Anti-Events

Effects fire at prediction time, not server confirmation time. A ring buffer of `TemporalEvents` (audio triggers, particle spawns, VFX handles) maps simulation events to presentation handles, timestamped by logic frame.

On rollback, a `PresentationReconciler` diffs the abandoned timeline against the corrected one:
- **Orphaned events** (predicted but wrong): issue an **Anti-Event** (RapidFadeOut, SoftCancel, RapidDecay)
- **New events** (in corrected timeline but absent from prediction): play now
- **Matching events**: no action

Anti-Events fade/decay over ~20ms rather than instant cull — visually continuous across rollback.

**Status: Designed, not yet implemented.** `AudioManager::FadeOut(handle, seconds)` is implemented and Anti-Event-compatible; the `PresentationReconciler` diff logic is not implemented.

---

## Memory Cost

| Component | Per-frame cost | Ring depth | Total |
|---|---|---|---|
| ECS slab (Temporal fields) | Varies by entity count | max(8, rollback_depth) | ~34MB at 100K entities |
| Jolt snapshots | ~7KB per physics frame (56 bodies) | Proportional to ring depth | Negligible |

At the 2026-03-29 validation point: ECS slab 34MB, Jolt state 7,436 bytes, byte-perfect memcmp verified.
