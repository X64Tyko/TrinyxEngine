# Game Flow (TrinyxEngine)

> **Navigation:** [← Architecture](ARCHITECTURE.md) | [← Status](STATUS.md) | [← Back to README](../README.md)

---

## 1. Overview / Purpose

This document defines the **session lifecycle, level transitions, player identity persistence, and
network-driven spawn flow** for TrinyxEngine. It is the implementation spec for the gameplay
framework layer that sits above the proven Construct/View OOP layer and Net subsystem.

The game flow system answers three practical questions:

1. **What survives a level transition?** Souls (player identity) and the Mode (match rules) survive.
   Bodies (world presence) do not.
2. **Who is authoritative over spawn?** The Mode validates every `PlayerBeginRequest`. The client predicts
   locally; the server confirms or rejects.
3. **How does the engine transition state cleanly?** A `FlowManager` owns a state stack, enforces
   declaration contracts, and drains a thread-safe event queue from the NetThread on the Sentinel
   thread.

---

## 2. Vocabulary / Key Concepts

Each concept maps directly to a concrete engine type:

| Concept   | Engine Type                                             | Description                                                                                                                                                                                                                                           |
|-----------|---------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **State** | `FlowState` subclass, managed by `FlowManager`          | A node in the session flow graph. Declares what it requires (World, NetSession, etc.). One active at a time, plus optional overlays. Examples: `MainMenuState`, `ConnectingState`, `LoadingState`, `InGameState`.                                     |
| **Mode**  | `GameMode` base class (opt-in `Construct<T>` for ticks) | Server-authoritative match rules. Validates spawn requests, manages rounds, picks spawn points. Owned by FlowManager. Users that need per-frame ticks also inherit `Construct<T>`.                                                                    |
| **Level** | Loaded `.tnxscene` identified by asset UUID             | Static geometry and level-placed entities. Loaded and unloaded within a World. The Level is data in the slab, not a class.                                                                                                                            |
| **Soul**  | `Soul` class (owned by FlowManager, NOT a Construct)    | Session-scoped player identity. Created by `FlowManager::OnClientLoaded`, destroyed by `OnClientDisconnected`. Holds `OwnerID`, `InputLead`, `ConfirmedBodyHandle`, `NetChannel`. When gameplay needs world presence, the Soul triggers a Body spawn. |
| **Body**  | `Construct<T>` with `ConstructLifetime::World`          | A Soul's world presence. Owns `ConstructView`s into ECS entities (the player character). Created by the Mode when a Soul enters gameplay. Destroyed when the World resets or the Soul leaves.                                                         |

> **Mental model:** State drives the app. Mode drives the match. Level drives the content. Souls
> persist identity. Bodies are world presence.

---

## 3. Construct Lifetime Tiers

Every Construct declares a static lifetime tier. `FlowManager` uses this to determine what survives
each transition.

```cpp
enum class ConstructLifetime : uint8_t
{
    Level,      // Destroyed when the Level unloads
    World,      // Destroyed when the World resets
    Session,    // Survives World reset. Destroyed when the session ends.
    Persistent, // Survives everything. Destroyed only explicitly.
};
```

Each Construct declares its tier as a `static constexpr` member:

```cpp
// Soul is NOT a Construct — it's a separate engine class owned by FlowManager.
// It has its own lifecycle tied to the network connection.

class PlayerBody : public Construct<PlayerBody>
{
public:
    static constexpr ConstructLifetime Lifetime = ConstructLifetime::World;
    ConstructView<EPlayer> PhysicsPresence; // invalidated on World reset
};

// GameMode is also NOT inherently a Construct. Users opt in via multiple inheritance
// when they need per-frame ticks:
class ArenaMode : public GameMode, public Construct<ArenaMode>
{
    // match rules, round state, spawn point registry
    void ScalarUpdate(SimFloat dt); // win condition check
};
```

### Transition Contract for Surviving Constructs

Constructs whose `Lifetime` is high enough to survive a transition receive two callbacks:

- **`OnWorldTeardown()`** — Views are about to be invalidated. Null out entity handles, View
  pointers, and Registry refs. Save any world-scoped state needed for reinitialization.
- **`OnWorldInitialized(World*)`** — A fresh World is ready. Reinitialize Views, re-create entities,
  and restore world presence.

Constructs that do **not** survive are destroyed by `FlowManager` before the transition executes.

### Ownership Enforcement

When `FlowManager` transitions to a state that does not require a World, all `Level`-lifetime and
`World`-lifetime Constructs are destroyed in declaration-reverse order. `Persistent`-lifetime
Constructs survive everything and are never destroyed automatically. Souls are separately owned
by FlowManager and survive World resets independently of the Construct lifetime system.

---

## 4. FlowManager and FlowState

### FlowManager

`FlowManager` is owned by `TrinyxEngine` and runs on the Sentinel thread. It manages:

- A **state stack** (push / pop / transition with declaration enforcement)
- Ownership of the **`ConstructRegistry`** — owned above `World` so Session-lifetime Constructs
  survive World destruction (see [Section 8](#8-ownership-hierarchy-revised))
- A **Souls array** indexed by OwnerID for Soul lookup
- A **thread-safe flow event queue** drained on the Sentinel thread once per frame

```cpp
class MyGame : public GameManager<MyGame>
{
public:
    bool PostInitialize(TrinyxEngine& engine)
    {
        auto& flow = engine.GetFlowManager();
        flow.RegisterState("MainMenu", [](){ return std::make_unique<MainMenuState>(); });
        flow.RegisterState("InGame",   [](){ return std::make_unique<InGameState>(); });
        flow.LoadDefaultState("MainMenu");
        return true;
    }
};
TNX_IMPLEMENT_GAME(MyGame)
```

### FlowState

`FlowState` is a base class with declaration hooks. Derived states override `GetRequirements()` to
declare what they need; `FlowManager` uses these declarations to know what to create and destroy
during transitions.

```cpp
class FlowState
{
public:
    virtual ~FlowState() = default;
    virtual void OnEnter(FlowManager& flow, World* world) {}
    virtual void OnExit() {}
    virtual void Tick(float dt) {}

    virtual StateRequirements GetRequirements() const { return {}; }
    virtual const char* GetName() const = 0;
};

struct StateRequirements
{
    bool NeedsWorld      = false;
    bool NeedsLevel      = false;
    bool NeedsNetSession = false;
    bool AllowsSouls     = true;
};
```

Example states:

```cpp
class MainMenuState : public FlowState
{
public:
    void OnEnter(FlowManager& flow, World* world) override { /* show main menu UI */ }
    void OnExit() override { /* hide UI */ }
    const char* GetName() const override { return "MainMenu"; }
    // GetRequirements() returns {} — no World needed
};

class InGameState : public FlowState
{
public:
    void OnEnter(FlowManager& flow, World* world) override { /* ask Mode to start match */ }
    void OnExit() override { /* signal World shutdown */ }
    StateRequirements GetRequirements() const override
    {
        return { .NeedsWorld = true, .NeedsNetSession = true };
    }
    const char* GetName() const override { return "InGame"; }
};
```

### Transition Logic

```
1. Compare current vs next state declarations (via GetRequirements()).
2. If current NeedsWorld and next does not:
       → Destroy World (destroys Level + all World-lifetime and Level-lifetime Constructs).
       → Persistent-lifetime Constructs stay on ConstructRegistry — untouched.
3. If current NeedsNetSession and next does not:
       → Destroy NetSession.
4. Call OnExit() on current state.
5. Call OnEnter(flow, world) on next state (World/NetSession created here if newly required).
```

---

## 5. FlowEvent — Network Flow Control

`FlowEvent` is a general-purpose network payload for flow control messages. It is added as a new
`NetMessageType` and routed by `NetThread::RouteMessage` to `FlowManager`'s event queue; it is
**never handled inline** on the NetThread.

```cpp
enum class FlowEventType : uint8_t
{
    LoadLevel,    // Payload: 16-byte asset UUID — server instructs client to load a level
    LoadAdditive, // Payload: 16-byte asset UUID — load on top of current level
    UnloadLevel,  // Payload: 16-byte asset UUID — unload a specific level
    KillWorld,    // No payload — destroy World, keep session
    KillSession,  // No payload — destroy session entirely
    ClientReady,  // Client → Server: finished loading, ready for spawn
    ServerReady,  // Server → Client: all initial spawns sent, simulation may start
};

struct FlowEventPayload
{
    uint8_t  EventType;      // FlowEventType discriminator
    uint8_t  Flags;          // Reserved; zeroed
    uint16_t PayloadSize;    // Trailing data size in bytes
    uint8_t  AssetUUID[16];  // Populated for Load/Unload events; zeroed otherwise
};
```

`FlowEvent` maps to `NetMessageType::FlowEvent = 7` in `NetTypes.h`. The full set of new message
types required by the game flow system is:

```cpp
// Additions to NetMessageType enum (after existing Pong = 6):
FlowEvent          = 7,   // Server↔Client: flow control (load level, kill world, ready signals)
PlayerBeginRequest = 8,   // Client→Server: request to spawn a character (Soul RPC)
PlayerBeginConfirm = 9,   // Server→Client: spawn accepted (NetHandle, auth position)
PlayerBeginReject  = 10,  // Server→Client: spawn rejected (reason code)
ClockSync          = 11,  // Bidirectional: clock synchronization probe
```

---

## 6. Network Connection Flow

The full connection sequence is divided into four phases. The `ClientRepState` (see
[Section 9](#9-clientrepstate)) advances through this flow per connection on the server.

### Phase 1 — Connection & Handshake

```
Client                              Server
  │
  ├── GNS Connect ──────────────────►│
  ├── HandshakeRequest ─────────────►│  version, client UUID
  │◄──────── HandshakeAccept ────────┤  OwnerID, tick rate, server frame#
  │
  │  PingProbe ×N ──────────────────►│  RTT measurement (N = ~8 probes)
  │◄──────── PongProbe ×N ──────────┤
  │
  ├── ClockSyncRequest ─────────────►│  client timestamp
  │◄──────── ClockSyncResponse ─────┤  server frame# + server timestamp
  │  Client computes input lead       │  (input lead = RTT/2 + jitter buffer)
  │
  ├── ClientReady ──────────────────►│
  │
  │  [RepState: PendingHandshake → Synchronizing]
```

### Phase 2 — Level Loading

```
  │◄──── FlowEvent::LoadLevel(uuid) ┤  server asks Mode for current level UUID
  │                                  │
  │  Client:                         │
  │  - Transitions to LoadingState   │
  │  - Creates World                 │
  │  - Loads .tnxscene from UUID     │
  │                                  │
  ├── FlowEvent::ClientReady ───────►│
  │◄──── FlowEvent::ServerReady ────┤  Mode decides when all initial spawns are done
  │                                  │
  │  [RepState: Synchronizing → Loading → Loaded]
```

### Phase 3 — Spawn (Client-Predicted)

```
  ├── PlayerBeginRequest ───────────►│  ClassID, desired position, PredictionID
  │                                  │
  │  Client predicts locally:        │  Server validates via Mode:
  │  - Creates Body Construct        │  - Mode::OnPlayerBeginRequest(soul, req)
  │  - Attaches ConstructView        │  - Mode picks authoritative spawn point
  │  - Links Soul.PendingBody        │  - Creates entity, replicates to others
  │                                  │
  │◄──── PlayerBeginConfirm ────────┤  NetHandle, frame#, auth position, PredictionID
  │  or                              │
  │◄──── PlayerBeginReject ─────────┤  reason code, PredictionID
  │                                  │
  │  If confirmed:                   │
  │  - Wire NetHandle → entity       │
  │  - Soul.ActiveNetHandle = handle │
  │  - Reconcile position if needed  │
  │                                  │
  │  If rejected:                    │
  │  - Destroy predicted Body        │
  │  - Soul.PendingBody = nullptr    │
  │                                  │
  │  [RepState: Loaded → Playing]
```

### Phase 4 — Playing

```
  ├── InputFrame (every logic tick) ►│  client sends input at logic rate
  │◄──── StateCorrection (batched) ──┤  unreliable, sent at NetworkUpdateHz
  │                                  │
  │  Client predicts locally          │  Server is authoritative
  │  Corrects on misprediction        │
  │  Heartbeat / RTT continuous       │
```

---

## 7. Travel — Level Transitions

Travel has three orthogonal levers that can be combined independently.

### Lever A — Domain Lifetime (What Survives at the World Level)

| Travel Type              | World | Level | Session |
|--------------------------|-------|-------|---------|
| Keep World, Swap Level   | ✓     | ✗     | ✓       |
| Reset World              | ✗     | ✗     | ✓       |
| Keep LocalSession only   | ✗     | ✗     | Soul only |

### Lever B — Construct Lifetime (What Survives by Tier)

| Lifetime Tier | Survives World Reset | Survives Level Swap |
|---------------|----------------------|---------------------|
| `Level`       | ✗                    | ✗                   |
| `World`       | ✗                    | ✓ (if World survives) |
| `Session`     | ✓                    | ✓                   |
| `Persistent`  | ✓                    | ✓                   |

### Lever C — Network Continuity

- **Keep NetSession** — same server, no reconnect (between-rounds map swap)
- **Swap NetSession** — server handoff (MMO zone transition, dedicated server rotation)

### FlowManager Transition Pseudocode

```cpp
void FlowManager::TransitionWorld(const char* newLevelUUID, WorldTransition transition)
{
    // 1. Collect Constructs that survive based on lifetime tier
    ConstructLifetime minSurvivor =
        (transition == WorldTransition::ResetWorld)
            ? ConstructLifetime::Session
            : ConstructLifetime::World;

    auto survivors = ConstructRegistry->GatherByMinimumLifetime(minSurvivor);

    // 2. Notify survivors: world is going away, null your Views
    for (auto* c : survivors) c->OnWorldTeardown();

    // 3. Detach survivors from tick batches on the old World
    for (auto* c : survivors) c->DetachFromWorld();

    // 4. Execute the transition (destroy old World / swap Level)
    ExecuteTransition(transition, newLevelUUID);

    // 5. Re-attach survivors to the new / refreshed World
    for (auto* c : survivors) c->AttachToWorld(ActiveWorld);

    // 6. Notify survivors: fresh World is ready, rebuild Views
    for (auto* c : survivors) c->OnWorldInitialized(ActiveWorld);
}
```

### Example Travel Scenarios

**Arena shooter — between rounds (Keep World, Swap Level):**
Mode sends `FlowEvent::LoadLevel(nextArenaUUID)`. Bodies (`World`-lifetime) are destroyed. Souls
(owned by FlowManager) survive. Mode resets round state in `OnWorldInitialized`. Souls request
new Bodies when `ServerReady` arrives.

**Roguelike — floor transition (Reset World):**
Mode calls `TransitionWorld(nextFloorUUID, ResetWorld)`. World is torn down. Persistent-lifetime
Constructs survive via `OnWorldTeardown` / `OnWorldInitialized`. Mode reinitializes its state.
New Bodies spawned when the client completes loading and sends `ClientReady`.

**MMO — server handoff (Swap NetSession + Reset World):**
Souls serialize their state to a transfer record. NetSession swaps to the new server. Souls
restore state in `OnWorldInitialized` on the destination server. Bodies spawn fresh.

---

## 8. Ownership Hierarchy

**`ConstructRegistry` is owned by `FlowManager`** (not `World`). This allows Persistent-lifetime
Constructs to survive World destruction. `World` continues to own the ECS `Registry`, physics,
and the simulation loop. Constructs that hold `ConstructView`s into a World have those Views
invalidated on World destruction and reinitialized on World creation via `OnWorldTeardown` /
`OnWorldInitialized`.

```
FlowManager  (manages the State stack + transitions)
  │
  ├── ConstructRegistry  (owns ALL Constructs regardless of lifetime tier)
  │     ├── Persistent-lifetime: MetaGameManager, CampaignTracker
  │     ├── World-lifetime:      PlayerBody(s), AIDirector
  │     └── Level-lifetime:      TurretController, DoorTrigger
  │
  ├── Souls[]  (Soul array indexed by OwnerID — owned by FlowManager, NOT Constructs)
  │
  ├── FlowState stack
  │     ├── active:   InGameState
  │     └── overlay:  PauseMenuState  (optional)
  │
  ├── NetSession  (optional — GNS context, NetThread, Connections)
  │
  └── World  (optional — ECS Registry, LogicThread, JoltPhysics, Level data)
```

`World` retains its existing `ConstructRegistry` accessor (`GetConstructRegistry()`) but the
pointer is now passed in from `FlowManager` at World initialization rather than owned by World
itself. This is a one-line change to `World::Initialize` and does not affect any existing callsites.

---

## 9. ClientRepState

Server-side per-connection state machine. Lives on a `ClientSession` struct (or an extension of the
existing `ConnectionInfo`) per connection on the server.

```cpp
enum class ClientRepState : uint8_t
{
    PendingHandshake, // GNS connected; waiting for HandshakeRequest
    Synchronizing,    // Handshake accepted; clock sync probes in flight
    Loading,          // FlowEvent::LoadLevel sent; waiting for ClientReady
    Loaded,           // Client loaded; waiting for Mode to approve spawn
    Playing,          // Full replication active; PlayerBeginConfirm sent
    Disconnecting,    // Graceful teardown in progress
};
```

State transitions are driven by network events on the server's NetThread and are forwarded to
`FlowManager`'s event queue for processing on the Sentinel thread.

---

## 10. Implementation Order

Recommended sequence — each step is independently compilable and testable:

1. **`ConstructLifetime` enum + `static constexpr Lifetime` on `Construct<T>`** — pure data addition,
   zero behavior change. `ConstructRegistry` can start filtering by tier immediately.

2. **`ConstructRegistry` owned by `FlowManager`** (not `World`) — pass the registry pointer into
   `World::Initialize`. Persistent/Session Constructs now survive `World` reset without any other changes.

3. **`FlowState` base class + `FlowManager`** — state stack, `GetRequirements()` declaration
   hook returning `StateRequirements`, transition logic (create/destroy World and
   NetSession as declarations change).

4. **`OnWorldTeardown()` / `OnWorldInitialized(World*)` callbacks on `Construct<T>`** — wired into
   `FlowManager::TransitionWorld`. Add to the same concept-detection pattern used for tick hooks.

5. **`FlowEvent` + `ClientRepState` types in `NetTypes.h`** — new enum values, `FlowEventPayload`
   struct, thread-safe queue on `FlowManager`.

6. **Handshake exchange in `NetThread::RouteMessage`** — parse `HandshakeRequest`, assign `OwnerID`,
   send `HandshakeAccept`, advance `ClientRepState` to `Synchronizing`.

7. **Clock sync probes** — RTT measurement using `Ping` / `Pong` messages (already defined in
   `NetMessageType`). Client computes input lead = RTT/2 + jitter buffer.

8. **`FlowEvent::LoadLevel`** — server sends UUID via `FlowEventPayload`, client creates World,
   loads `.tnxscene`, sends `FlowEvent::ClientReady`. Server advances `ClientRepState` to `Loaded`.

9. **`PlayerBeginRequest` / `PlayerBeginConfirm` / `PlayerBeginReject`** — client sends `PlayerBeginRequest` with a
   `PredictionID`, predicts locally by creating a `Body`. Server calls
   `Mode::OnPlayerBeginRequest(soul, req)`, sends `PlayerBeginConfirm` (with authoritative position + NetHandle)
   or `PlayerBeginReject`. Client wires or destroys the predicted Body accordingly.

10. **Misprediction detection + correction-triggered rollback** — on `PlayerBeginConfirm`, compare
    authoritative position to predicted position. If delta exceeds threshold, trigger rollback to
    the confirmation frame and resimulate.

---

## 11. Open Questions / Future Work

- **Async level loading** — the current blocking `Spawn()` lambda is acceptable for R&D. A worker
  thread path is required before shipping to avoid stalling the Sentinel thread during large scene
  loads.
- **Delta compression for `StateCorrection`** — currently full-state every net tick. Pairs naturally
  with the existing dirty-bit tracking infrastructure.
- **Interest management / relevancy filtering** — server sends all entities to all clients today.
  Per-connection relevancy sets are required before large-scale multiplayer can be tested.
- **Spectator mode** — a Soul with no Body that receives full replication. `ClientRepState::Playing`
  already covers this; Mode needs a spectator path in `OnPlayerBeginRequest`.
- **Soft server handoff** — overlapping NetSessions during the transition window, so packets in
  flight from the old server are still processed correctly.
- **`FlowState` overlays** — pause menus, loading screens, and HUDs as pushable overlay states
  that do not trigger full transition logic.
