# Networking Overview

> [Home](../Home.md) | [Connection Flow →](Connection-Flow.md)

---

## Model

Server-authoritative over GameNetworkingSockets (GNS). The transport is an implementation detail — gameplay-facing API is built around `NetChannel`, `Soul`, and `ConstructHandle`. PIE loopback is the primary development target; dedicated server follows naturally from the same code paths.

---

## Vocabulary

**Never use "server" or "client" as standalone nouns.** Use these terms:

| Term | Meaning |
|---|---|
| `Authority` | Dedicated server, or the authoritative sim side of a Host |
| `Owner` | The local player; the Soul that owns input for an entity |
| `Host` | Listen server — Soul with both `Authority + Owner` roles (`EngineMode::Host`) |
| `Echo` | Non-owning entity stance on a client (remote player's representation) |
| `Solo` | Offline, no networking (`EngineMode::Standalone`, `SoloSim`) |
| `AuthorityNet` | Authority-side net handler (`AuthorityNet.h`) |
| `OwnerNet` | Owner-side net handler (`OwnerNet.h`) |

---

## Two Network Modes

### Deterministic Mode

Frame numbers are the primary packet identifier. Input replication is the sync mechanism. State replication is optional — useful only as a divergence correction signal.

- All peers must run at the same logic Hz
- Slow clients are a policy problem, not an architectural one: rollback fast-forwards them back; disconnect policy handles unrecoverable deficit
- Suited for competitive play where simulation integrity is paramount

### Non-Deterministic Mode

Server timestamps (`server_time_us`) replace frame numbers as the primary packet identifier. State replication is the primary sync signal — not optional in practice, since clients cannot guarantee identical outcomes.

- Clients can run at any logic frame rate
- Uncapped renderer interpolates smoothly between 30Hz snapshots regardless of local frame rate
- Suited for co-op, casual games, or games with variable client performance expectations

The `TNet` policy axis encodes which mode a given build uses. `AuthoritySim` and `OwnerSim` currently implement Deterministic Mode only. Non-Deterministic Mode requires a separate rewrite of those paths — see [Networking Without Rollback](#networking-without-rollback--current-state-and-direction) and [Known Gaps](#known-gaps).

---

## Core Components

### GNSContext

Thin wrapper around GNS library init/teardown. Isolates GNS headers from the rest of the engine. Statically linked. One per process lifetime.

### NetConnectionManager

Socket API: `Listen(port)` / `Connect(address, port)`, `PollIncoming()` / `Send()`.

Per-connection state (`ConnectionInfo`): GNS handle, OwnerID, sequence numbers, RTT, ack bitfield, `ClientRepState` machine. Max simultaneous connections = 2^`NetOwnerID_Bits` (currently 8 bits → 256 connections).

### NetThread

Dedicated network poller. Default rate: **30Hz** (configurable via INI `NetworkUpdateHz`). Runs `HandleMessage` dispatch loop.

| Message | Direction | Handler |
|---|---|---|
| `ConnectionHandshake` | Owner→Authority | Assign OwnerID, send ack |
| `InputFrame` | Owner→Authority | Route to World `GetSimInput()` by OwnerID |
| `EntitySpawn` | Authority→Owner | `ReplicationSystem::HandleEntitySpawn` |
| `StateCorrection` | Authority→Owner | `ReplicationSystem::HandleStateCorrections` |
| `FlowEvent` | Authority→Owner | `FlowManager::PostNetEvent` (TravelNotify, ServerReady) |
| `ClockSync` | Both | RTT + clock offset estimation |
| `PlayerBeginRequest` | Owner→Authority | Soul RPC: Owner requests spawn |
| `PlayerBeginConfirm` | Authority→Owner | Soul RPC: Authority confirms spawn |
| `PlayerBeginReject` | Authority→Owner | Soul RPC: Authority rejects spawn |
| `Ping/Pong` | Both | RTT EWMA (0.875/0.125 weights) |

### NetChannel

Per-connection typed send API — the natural home for all future per-connection state (delta compression baselines, coalescing buffers, reliability policy tables, RPC dispatch).

```cpp
class NetChannel
{
    uint8_t OwnerID;

    template<typename T>
    void Send(NetMessageType type, const T& payload);

    template<typename T>
    void SendTo(uint8_t targetOwnerID, NetMessageType type, const T& payload);

    void Flush();
};
```

All gameplay messages route through the Authority. The Owner specifies a target `OwnerID`; the Authority relays without inspecting the payload. This is addressed routing, not multicast — no "everyone filters it" overhead.

---

## `LogicThread<TNet, TRollback, TFrame>` — Sim Mode Policy

`LogicThread` is templatized on three policy axes to eliminate all in-line Authority/Owner branching:

```cpp
template <typename TNet, typename TRollback, typename TFrame>
class LogicThread : public LogicThreadBase { ... };
```

**Net policy types:**

| Policy | Shorthand | Role |
|---|---|---|
| `AuthoritySim` | AuthNet | `OnSimInput` injects per-player `PlayerInputLog` from `ServerClientChannel`; `OnFramePublished` advances `CommittedFrameHorizon` |
| `OwnerSim` | OwnerNet | `OnSimInput` pushes to `InputAccumRing`; `OnFramePublished` is a no-op |
| `SoloSim` | — | Both are no-ops |
| `ListenSim` *(planned)* | ListenNet | Host-is-a-peer; combines Authority and Owner roles; runtime authority override table resolves per-entity ownership; migration-capable |

`AuthNet` and `OwnerNet` are stripped builds — no authority surface is exposed to the wrong side. The security boundary is enforced at compile time, not runtime. `ListenNet` carries overhead the other modes never pay: runtime authority resolution, migration state, and both frame paths simultaneously.

Dead code never compiles in a given build. Add new Authority/Owner branching to the sim mode policy — not to `LogicThread` directly.

---

## `ServerClientChannel` — Per-Client Replication State

One per connected Owner. Replaces a single global `Replicated[]` bitvector. O(1) OwnerID lookup.

```cpp
struct ServerClientChannel {
    PlayerInputLog        InputLog;            // inbound input frames from this Owner
    std::vector<bool>     Replicated;          // per-entity spawn-tracking
    std::vector<uint32_t> PendingActivations;  // net handles queued for EntityActivate
    NetChannel            Channel;             // typed per-connection send wrapper
    PendingPacketQueue    SendQueue;           // lock-free MPSC; drained by Sentinel
    ConnectionInfo*       CI;
    uint8_t               OwnerID;
    uint32_t              LastAckedSimFrame;
};
```

`ReplicationSystem` holds `Clients[MaxOwnerIDs]` for O(1) lookup. Lives inside the `World` it belongs to — PIE worlds are naturally isolated.

---

## Network Identity

### EntityNetHandle

Packed `uint32_t`: `NetOwnerID:8 | NetIndex:24`. OwnerID 0 = Authority/global, 1–255 = Owners. NetIndex allocated per entity on the Authority. `NetToRecord[]` maps NetIndex → `EntityRecord`.

**Three handle spaces in Registry:**

| Handle | Space | Purpose |
|---|---|---|
| `GlobalEntityHandle` | Internal | Generation + Records[] index |
| `EntityHandle` | Local OOP | LocalToRecord mapping |
| `EntityNetHandle` | Network | NetToRecord mapping, wire-safe |

### EntityRef vs EntityNetHandle (Two-Tier)

**Internal (32-bit) `EntityNetHandle`:** Used exclusively inside engine systems. Compact, fast, no generation. Valid on bulk paths where the Authority is pushing state.

**External (64-bit) `EntityRef`:** Used in gameplay code, RPCs, and all Owner→Authority messages. Embeds the generation captured at spawn time for ABA protection.

```cpp
struct EntityRef {
    EntityNetHandle Handle;      // 32-bit wire handle
    uint16_t        Generation;  // from GlobalEntityHandle::Generation at spawn
    uint16_t        Flags;       // IsPredicted:1, IsOwned:1, reserved:14
};  // 8 bytes total
```

When an Owner sends a targeted message, the Authority validates the generation before any state mutation. Stale references are rejected.

### ConstructHandle (Planned)

Pure-logic Constructs (Soul, GameMode, AIDirector) have no `EntityNetHandle` — they're not entities. `ConstructHandle` fills this gap:

```cpp
struct ConstructHandle {  // fits in uint32_t
    uint8_t  OwnerID;     // Soul that owns this Construct (0 = Authority-owned)
    uint16_t LocalIndex;  // index into that owner's ConstructRegistry PagedMap
    uint8_t  Generation;  // stale-handle detection
};
```

RPCs targeting a Construct as a whole use `ConstructHandle`. Multi-entity Constructs (Turret with barrel + base entities) have multiple `EntityNetHandle`s but one `ConstructHandle`.

---

## PIE Loopback

Editor creates Authority + N Owner Worlds in the same process with loopback GNS connections.

- Each Owner World gets its own OwnerID, viewport, field slab, and InputBuffer
- Authority World runs headless (no renderer)
- Input routes to the focused viewport's World via `InputTargetWorld`
- Both `AuthorityNet::HandleMessage` and `OwnerNet::HandleMessage` run in-process; `WorldMap[ownerID]` routes messages to the correct World

PIE is not a simulation of networking — it IS networking over a loopback socket. Bugs found in PIE are real bugs.

**PIE disambiguation note:** In PIE, two `ConnectionInfo` entries share the same OwnerID (one `bServerSide=true`, one `bClientInitiated=true`). `FindConnectionByOwnerID` must be called with `requireServerSide=true` when the Authority side needs to set state on its own leg.

---

## Gated Push Replication

```
PublishCompletedFrame → OnFramePublished → dispatch one read-only job per Loaded+ client
```

Each job owns its `ServerClientChannel` exclusively — zero contention. Replication state correction and spawn sends are read-only against the ECS slab; they run async alongside the render thread. Logic rate (512Hz) and net rate (30Hz) are decoupled.

---

## Disconnect Policy

The engine surfaces health metrics and executes whatever action the game returns. Policy decisions are never owned by the engine itself.

```cpp
struct ClientHealthMetrics {
    float    packet_loss_pct;
    uint32_t ms_behind_server;
    uint32_t consecutive_missed_inputs;
    uint32_t ticks_behind;
    uint64_t last_packet_time_us;
    bool     clock_sync_valid;
};

enum class ClientAction { None, Warn, Disconnect, Pause };

using ClientHealthCallback = ClientAction(*)(
    ClientID, ClientHealthMetrics, void* user_data
);
```

`NetDisconnectPolicy` provides sane defaults for games that don't implement a custom callback:

```cpp
struct NetDisconnectPolicy {
    uint32_t timeout_ms               = 10000;
    float    packet_loss_threshold    = 0.30f;
    uint32_t max_ticks_behind         = 512;   // deterministic mode
    uint32_t max_ms_behind            = 500;   // non-deterministic mode
    uint32_t health_check_interval_ms = 1000;
    ClientHealthCallback     on_health_changed = nullptr;
    ClientDisconnectCallback on_disconnect     = nullptr;
};
```

A null callback uses default threshold enforcement. Setting the callback grants full control over Warn/Disconnect/Pause decisions.

**Anti-cheat surface:** The disconnect policy incidentally bounds the exploitation window for common cheats:
- Lag switches are capped to the variance window tolerance
- Speed hacks: inputs timestamped ahead of server time are flagged as anomalous
- Position hacks: the Authority owns simulation — client position is never trusted
- Input injection: 128Hz input cap enforced Authority-side

**Status: Designed, not yet implemented.**

---

## Host Migration (ListenNet)

Host migration is a `ListenNet`-only concern. `AuthNet` (dedicated server) has no migration path; `OwnerNet` is a client-only build.

### AuthorityClass Tags

Each entity in a ListenNet game carries an `AuthorityClass` tag that governs authority transfer:

| Tag | Meaning |
|---|---|
| `Host` | Follows the current host; transfers on migration |
| `Owner` | Owned by the spawning peer; survives migration naturally |
| `Fixed` | Compile-time permanent authority; never transfers |

### Prerequisites Already Satisfied

Migration requires no new infrastructure — only assembly of existing parts:
- **Temporal ring buffer** — every peer has recent state history available as a snapshot
- **Base frame offset per client** — re-anchoring to a new host is recalculating the offset
- **Health metrics** — a degrading host is detectable before full disconnection
- **Input windowing** — already active; retransmit input stream to new host to cover the gap

### Handshake Protocol

1. Old host continues simulating and acting as Authority during the handshake window
2. Old host sends explicit state and input stream to the migration candidate
3. New host fast-forwards via rollback until its snapshot matches
4. New host signals ready and notifies all connected peers
5. Old host receives confirmation and stops sending state snapshots
6. Authority transfer complete — peers experience no gap

### Candidate Election

- Ranked by latency and snapshot recency; freshest snapshot wins ties
- Old host does not relinquish authority until it receives explicit confirmation from the new host that all peers have been notified

### Handshake Timeout and Candidate Failure

If the old host detects candidate loss: abort handshake, reassert full authority, select next ranked candidate, restart handshake.

### Memory Constraint

No logical host/client split exists internally — preallocated slabs make doubling the footprint unreasonable. Migration serializes into snapshot format and transmits to the new host, which deserializes into its own preallocated slab. This serialization path also serves save states, late join, reconnect, and debug replay.

**Status: Designed, not yet implemented.** Requires the snapshot serialization path first.

---

## Networking Without Rollback — Current State and Direction

Deterministic lockstep with rollback is implemented and proven. Games that don't want rollback — co-op, casual, or
titles with variable client performance — currently get the lockstep path with rollback compiled out (`NoRollback`),
**not** a purpose-built non-deterministic mode. The holes below are known; each lists the intended direction.

| # | Hole | Where | Effect | Intended direction |
|---|---|---|---|---|
| 1 | **No reconciliation for the controlled entity.** With rollback off, a `StateCorrection` (already RTT/2 old) is written straight into the current write frame for every entity, including the local player's. Unacknowledged inputs are never replayed. | `OwnerNet::HandleStateCorrections` (non-rollback branch) | Rubber-banding proportional to latency. `CVisualTransform` hides the pop, not the pull-back. | Owner-only prediction ring: keep input + state history for predicted entities only; on correction for frame F, restore those entities to the server state at F and replay inputs F+1..now. Same machinery as the planned dirty-propagation resim, applied to a handful of entities. |
| 2 | **One lagging client stalls the whole server.** `AuthoritySim` freezes the sim for everyone when any player's input is more than `MaxClientInputLead` frames behind (default 16 ≈ 31ms), in every build mode. | `AuthoritySim::RunSimInput` | One bad connection or slow machine freezes all players. Correct for lockstep, wrong otherwise. | The stall becomes a lockstep-only policy. Other modes use a per-client missing-input policy (repeat last input for a bounded window, then idle) and never stall globally. |
| 3 | **Late input is recorded but not acted on.** The server always keeps a rolling per-player input log (`PlayerInputLog`, one slot per sim frame, depth = `TemporalFrameCount`). Late input lands in the log and requests a rollback — a no-op under `NoRollback`, so the server keeps its predicted input for that frame. | `AuthoritySim::RunSimInput`, `PlayerInputLog` | The client's real input never affects the authoritative sim; divergence surfaces as corrections (and rubber-banding via #1). | **Server-only rollback** (below): the server re-simulates from its input log while clients stay rollback-free. |
| 4 | **Fixed 512Hz sim, one-time clock sync.** A client that can't sustain 512Hz runs 8 catch-up steps, then discards time beyond 0.25s and falls behind real time. `FrameOffset` / `InputLead` are set once at the handshake and never adjusted. | `LogicThread` accumulator, `NetThreadBase::TickClockSync` | Only the render rate can vary. A slow or drifting client pushes the server into #2 or #3. | Continuous clock sync and client time dilation (small speed-up / slow-down to hold a target input lead). The variable-rate model is an open decision: fixed sim rate with integer divisors for weak clients (keeps frame math integer and replay simple) vs. timestamp-based variable rate (`SimFrame`, designed but not wired into `PacketHeader`). |
| 5 | **No lag compensation.** No server-side rewind for hit validation; without rollback the server has no history deeper than the 3-frame Volatile buffer. | — | Hitscan and melee can't be validated fairly at real-world latency. | Server keeps hit-validation history — naturally provided by server-only rollback's Temporal ring. Tied to the open remote-representation design below. |
| 6 | **Remote entity representation is open.** Echo entities take corrections as they arrive, smoothed only by `CVisualTransform` easing. No buffered interpolation, no extrapolation policy. | `CVisualTransform` | Uneven motion under jitter. | **Open design** — see [Remote Entity Representation](#open-design-remote-entity-representation). |
| 7 | **Physics-driven owned entities** get a Jolt teleport override on each correction, with no prediction replay. | Correction path | Jitter; corrections fight the solver. | Deferred until the Trinyx physics solver — see [Physics](../gameplay/Physics.md#long-term-trinyx-physics-solver). |
| 8 | **Discrete input events aren't injected on the server** — only held-key state. | `AuthorityNet.cpp` (`TODO: inject discrete events`) | Mostly harmless at 512Hz input frames; a trap for press/release-triggered actions. | Inject per-frame discrete events from the input log. |
| 9 | **`PlayerInputLog` is unsynchronized.** NetThread writes and the Logic thread reads without a lock; safe only while their phases don't overlap. | `PlayerInputLog.h` header note | Latent data race. | Lightweight per-log spinlock or a lock-free handoff. |

### Server-Only Rollback (Planned)

The Authority can run with rollback enabled while Owners run without it. The server keeps the Temporal ring, Jolt
snapshots, and its per-player input log, and re-simulates when late input arrives. Owners run the non-rollback
prediction-and-replay path (#1) and apply corrections normally; they already ignore the `ResimFrameDelta` annotation
when rollback is off.

Server re-simulation only has to reproduce the server's **own** results (same process, same binary), not match other
machines bit-for-bit, so server-only rollback doesn't require `TNX_DETERMINISM` — and the two flags are already
independent in the build. The PIE server already runs this way (editor builds force rollback on). What remains is
wiring shipped server builds with rollback against client builds without it.

The same server history supports lag compensation (#5).

### Open Design: Remote Entity Representation

Every client sees remote players in the past: network latency plus whatever interpolation delay smooths their motion.
Players end up aiming at a **shadow of the target**. The standard fix — lag compensation that rewinds the server to what
the shooter saw — favors the shooter and moves the unfairness onto the target: shots that land after the target reached
cover, and peeker's advantage.

Trinyx intends to address this rather than adopt snapshot interpolation plus lag compensation as-is. The engine has
unusual tools to work with: a 512Hz simulation, microsecond-timestamped inputs, a rolling per-player input log on the
server, Temporal history, and rollback. **Status: open — not yet designed.** Buffered interpolation, extrapolation
policy, and lag-compensation rules are deliberately left undecided until this design exists.

---

## Known Gaps

- No interest management / relevancy culling — Authority sends all entities to all Owners today
- Phase 0 tentative despawn not implemented — see [Despawn Protocol](Despawn-Protocol.md)
- `ListenNet` (`ListenSim`) TNet policy not yet implemented — `AuthorityClass` tagging and runtime override table pending
- Host migration designed but not implemented — requires snapshot serialization path
- Disconnect policy designed but not implemented — `ClientHealthMetrics` structs not wired
- Deterministic/Non-Deterministic mode split is a planned netcode rewrite; current code is Deterministic only. Holes in the rollback-disabled path, server-only rollback, and the open remote-representation design are listed in [Networking Without Rollback](#networking-without-rollback--current-state-and-direction)
