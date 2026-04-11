# Networking Architecture

> **Navigation:** [← Architecture](ARCHITECTURE.md) | [← Game Flow](FLOW.md) | [← Back to README](../README.md)

---

## Overview

Server-authoritative model over GameNetworkingSockets (GNS). The transport is an implementation detail —
the gameplay-facing API is built around `NetChannel`, `Soul`, and `ConstructHandle`. PIE loopback is the
primary development target; dedicated server follows naturally from the same code paths.

---

## Core Components

### GNSContext

Thin wrapper around GNS library init/teardown. Isolates GNS headers from the rest of the engine.
Statically linked. One per process lifetime.

### NetConnectionManager

Server/client socket API. `Listen(port)` / `Connect(address, port)`, `PollIncoming()` / `Send()`.

Per-connection state (`ConnectionInfo`): GNS handle, OwnerID, sequence numbers, RTT, ack bitfield,
`ClientRepState` machine. Max simultaneous connections = 2^`NetOwnerID_Bits` (currently 8 bits → 256).

### NetThread

Dedicated network poller. Default rate: **30Hz** (configurable via INI `NetworkUpdateHz`).
Runs `HandleMessage` dispatch loop, routes by `NetMessageType`:

| Message               | Direction | Handler                                               |
|-----------------------|-----------|-------------------------------------------------------|
| `ConnectionHandshake` | C→S       | Assign OwnerID, send ack                              |
| `InputFrame`          | C→S       | Route to World::GetSimInput() by OwnerID              |
| `EntitySpawn`         | S→C       | ReplicationSystem::HandleEntitySpawn                  |
| `StateCorrection`     | S→C       | ReplicationSystem::HandleStateCorrections             |
| `FlowEvent`           | S→C       | FlowManager::PostNetEvent (TravelNotify, ServerReady) |
| `ClockSync`           | S↔C       | RTT + clock offset estimation                         |
| `SpawnRequest`        | C→S       | GameMode::OnSpawnRequest (planned)                    |
| `SpawnConfirm`        | S→C       | Client creates PlayerConstruct (planned)              |
| `SpawnReject`         | S→C       | Client handles rejection (planned)                    |
| `Ping/Pong`           | both      | RTT EWMA (0.875/0.125 weights)                        |

### ReplicationSystem

Server-side entity replication. Runs each net tick:

1. `SendSpawns()` — reliable `EntitySpawn` for entities not yet replicated to each client
2. `SendStateCorrections()` — unreliable batched transforms for all live entities
3. After flushing all spawns: sends `FlowEvent::ServerReady` → client sweeps `Alive→Active`

---

## NetChannel (Planned)

The current pattern — manually building `PacketHeader` inline at every send site — does not scale.
`NetChannel` replaces it with a per-connection typed send API that is the natural home for all future
per-connection state.

### Design

```cpp
class NetChannel
{
public:
    uint8_t OwnerID;   // Which Soul this channel belongs to (server=0)

    // Typed send — builds PacketHeader, enqueues or sends immediately.
    template<typename T>
    void Send(NetMessageType type, const T& payload);

    // Flush all coalesced sends (no-op until coalescing is implemented).
    void Flush();

    // Route a message to a specific target Soul via the server relay.
    // Server knows target's connection; client specifies intent, server routes.
    template<typename T>
    void SendTo(uint8_t targetOwnerID, NetMessageType type, const T& payload);

private:
    HSteamNetConnection Connection;
    ISteamNetworkingSockets* Sockets;
};
```

### What lives here over time

- **Delta compression state** — last-acked field values per entity, per channel
- **Message coalescing** — buffer sends within a tick, flush once at frame end
- **Reliability selection** — per-message-type reliable/unreliable/reliable-no-nagle policy
- **RPC dispatch table** — `ConstructHandle` → method ID → local handler

### Addressed routing (not multicast)

Clients specify a target `OwnerID`. The server routes to that specific peer's connection.
No multicast-then-filter pattern. The sender does not need to know the target's connection handle —
only the OwnerID. `NetChannel::SendTo(targetOwnerID, ...)` makes the intent explicit in code.

### Direct peer-to-peer — capability noted, deferred

GNS supports direct peer connections and every client has a stable `OwnerID`. Direct P2P would
eliminate one server hop for latency-sensitive messages (e.g., hit confirmation between two clients).

**Why deferred:** Clients sending authoritative gameplay events directly to each other is exploitable
in competitive play without a separate server validation pass. The capability exists in the transport
but enabling it requires a dedicated security design (authority model, validation, rate limiting) before
it is safe to expose.

**What we do instead:** All gameplay messages route through the server. The client specifies a target
(`targetOwnerID`) — the server relays without inspecting the payload. This is addressed, not multicast,
so there is no "everyone filters it" overhead. The API (`SendTo`) is forward-compatible with direct P2P
when that security design is complete.

---

## Fast-Path RPC (Planned, Post-Hardening)

The 30Hz net tick rate imposes a 33ms ceiling on event-driven gameplay. Some events (hit detection,
input acknowledgment) are latency-sensitive enough to warrant bypassing the tick rate.

**Proposed design:** A second polling loop on the NetThread at **1KHz** (matching the Sentinel input
thread) for size- and frequency-limited "immediate" messages. These bypass the 30Hz coalescing window
and are dispatched inline.

**Constraints (to prevent abuse):**

- Maximum payload size (e.g., 64 bytes)
- Per-channel rate limit (e.g., max 4 immediate messages per logic frame)
- Only specific `NetMessageType` values are eligible for fast-path

**Target use cases:** Input frame delivery, hit registration events, animation trigger replication.

**Status:** Not implemented. Requires careful replay-proof design and testing.

---

## Per-Object Net Tick Rates (Future)

Not all networked objects require the same update frequency. An ambient prop needs far fewer state
corrections than a player character. Future work: per-archetype or per-entity `NetTickDivisor` that
controls how often `ReplicationSystem` includes an entity in a `StateCorrection` batch.

High-priority objects (players, projectiles): every net tick.
Medium-priority (AI, vehicles): every N ticks.
Low-priority (props, environment): on-change only (dirty bit driven).

---

## Network Identity

### Two-Tier Handle Design

All networked objects have two handle widths serving different purposes:

**Internal (32-bit) — `EntityNetHandle` / `ConstructNetHandle`**
Used exclusively inside engine systems: ReplicationSystem, NetThread, Registry.
Compact, fast, no generation. Valid on bulk paths where the server is the authority
(e.g., `StateCorrectionEntry` stays 32-bit — the server is pushing state, not validating
a client reference).

**External (64-bit) — `EntityRef` / `ConstructRef`**
Used in gameplay code, RPCs, and all client→server messages. Embeds the generation
captured at spawn time for ABA protection. When a client sends a targeted message,
the server validates:

```
NetToRecord[ref.Handle.NetIndex].Generation == ref.Generation
```

Stale references (recycled slot) are rejected before any state mutation.

```cpp
struct EntityRef {
    EntityNetHandle Handle;      // 32-bit wire handle
    uint16_t        Generation;  // from GlobalEntityHandle::Generation at spawn
    uint16_t        Flags;       // IsPredicted:1, IsOwned:1, reserved:14
};  // 8 bytes total

struct ConstructRef {
    ConstructNetHandle Handle;
    uint16_t           Generation;
    uint16_t           Flags;
};  // 8 bytes total
```

`EntityGeneration_Bits` in `RegistryTypes.h` controls the generation width. If it ever
exceeds 16, the `static_assert` on `EntityRef` catches the mismatch before silent breakage.

### SpawnFlags Generation Packing

`EntitySpawnPayload::SpawnFlags` carries the entity's generation alongside the spawn flags
in a single `int32_t`, avoiding payload size growth:

```
bits [31 : 32-EntityGeneration_Bits]  =  entity generation
bits [EntityGeneration_Bits-1 : 0]   =  spawn flags (Active, Background, etc.)
```

Use `EntitySpawnPayload::Pack(flags, generation)` on the server and
`GetFlags(spawnFlags)` / `GetGeneration(spawnFlags)` on the client.
The client uses `GetGeneration()` to construct a valid `EntityRef`.
The client uses `GetFlags()` to write into `CacheSlotMeta` — never writes the raw value.

### EntityNetHandle

Packed `uint32_t`: `NetOwnerID:8 | NetIndex:24`. OwnerID 0 = server/global, 1-255 = clients.
NetIndex allocated per entity on the server. `NetToRecord[]` maps NetIndex → `EntityRecord`.

**Three handle spaces in Registry:**

| Handle               | Space     | Purpose                        |
|----------------------|-----------|--------------------------------|
| `GlobalEntityHandle` | Internal  | Generation + Records[] index   |
| `EntityHandle`       | Local OOP | LocalToRecord mapping          |
| `EntityNetHandle`    | Network   | NetToRecord mapping, wire-safe |

### ConstructHandle (Planned)

Constructs (PlayerConstruct, GameMode, AIDirector) are not entities — they have no `EntityNetHandle`.
But RPCs and spawn confirmations need to address them. `ConstructHandle` fills this gap.

```
struct ConstructHandle {   // fits in uint32_t
    uint8_t  OwnerID     — Soul that owns this Construct (0 = server-owned)
    uint16_t LocalIndex  — index into that owner's ConstructRegistry PagedMap
    uint8_t  Generation  — stale-handle detection, bumped on Construct destroy
};
```

**Design principles, modeled on `EntityRecord`/`PagedMap`:**

- `ConstructRegistry` gains a `PagedMap<MaxConstructs, ConstructRecord>` backing store.
  `ConstructRecord` holds: typed pointer, generation, type hash, OwnerID.
- Lookup validates generation first — stale handle returns null, never UB.
- `LocalIndex` is owner-local. Peer A's index 7 ≠ peer B's index 7. The OwnerID disambiguates.
  No requirement for indices to match across connections.
- Wire representation: `OwnerID` is globally stable (assigned at connection). `LocalIndex` resolves
  on each peer independently via its own ConstructRegistry.

**Pure-logic Constructs** (Soul, GameMode, AIDirector) have no entity. They are completely unreachable
via `EntityNetHandle`. `ConstructHandle` is the only way to address them on the wire.

**Multi-entity Constructs** (Turret with barrel + base entities) have multiple `EntityNetHandle`s but
one `ConstructHandle`. RPCs targeting the Construct as a whole use the `ConstructHandle`.

---

## Soul / OwnerID Model

**One Soul per OwnerID, always.** Even splitscreen: two players on one machine = two Souls = two
OwnerIDs. The machine holds one `NetChannel` per Soul. The `NetChannel` knows its OwnerID and the
connection to the server. The OwnerID is the stable identity for the entire session.

Soul lifecycle:

1. Client connects → server assigns OwnerID → both sides create Soul for that OwnerID
2. Client loads level, sends `LevelReady` → server sends `ServerReady`
3. Client sweeps `Alive→Active`, sends `SpawnRequest`
4. Server: `GameMode::OnSpawnRequest(soul)` → spawn PlayerConstruct → send `SpawnConfirm`
5. Client receives `SpawnConfirm` → create local PlayerConstruct at auth position
6. Client disconnects → both sides destroy Soul, call `GameMode::OnPlayerLeft(soul)`

`FlowManager` owns the Soul array: `Soul* Souls[MaxOwnerIDs]`. Keyed by OwnerID.
`GameMode` hooks: `OnPlayerJoined(Soul&)`, `OnPlayerLeft(Soul&)`, `OnSpawnRequest(Soul&)`.

---

## PIE Loopback

Editor creates server + N client Worlds in the same process with loopback GNS connections.
Each client World gets its own OwnerID, viewport, field slab, and InputBuffer.
Server World runs headless (no renderer).
Input routes to the focused viewport's World via `InputTargetWorld`.
Both `ServerNetThread::HandleMessage` and `ClientNetThread::HandleMessage` run in the same process;
`WorldMap[ownerID]` routes messages to the correct World.

---

## Replay & Recording

The SoA layout makes replay nearly free. All deterministic simulation state lives in contiguous,
trivially-copyable field arrays. Replay = serialization in `PropagateFrame`.

**Compression advantage:** Homogeneous float arrays (all PosX, all PosY) have high spatial coherence.
Delta compression yields mostly zeros per tick. Far superior to AoS where mixed types destroy ratios.

**Free wins from slab layout:**

- Kill cam / rewind — slab snapshots in ring buffer, rewind = index backward + re-render
- Spectator scrubbing — random access seek via snapshot index
- Anti-cheat — diff server vs client slab timelines offline
- Bandwidth floor — compressed delta = theoretical minimum sync payload
- Sub-tick precision — input events timestamped at actual ms, not frame-quantized
- With rollback — retroactive event insertion enables frame-perfect multiplayer reproduction

---

## Known Gaps

- No delta compression (full state every tick)
- No interest management / relevancy culling
- No entity destruction replication
- `SpawnRequest`/`SpawnConfirm`/`SpawnReject` pipeline not yet wired
- `NetChannel` not yet implemented (raw `HSteamNetConnection` used inline)
- `ConstructHandle` not yet implemented
- Soul not yet implemented (forward-declared in GameMode.h only)
