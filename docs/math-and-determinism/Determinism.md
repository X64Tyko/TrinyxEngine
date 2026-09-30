# Determinism

> [← Fixed-Point](Fixed-Point.md) | [Home](../Home.md)

---

## Scope

Determinism applies to **authoritative simulation state**:

- Entity component data in slab-backed tiers (Temporal / Volatile / Static / Cold where applicable)
- Allocation/free behavior that affects `EntityCacheIndex` stability
- Any gameplay-affecting math in the fixed update

Determinism does **not** apply to:
- Rendering outputs (GPU results)
- Editor-only state and transient UI state
- Performance timing and scheduling (unless it changes simulation outcomes)

---

## How Determinism Is Enforced

Determinism is primarily enforced by **architecture**, not developer discipline:

| Mechanism | How it enforces determinism |
|---|---|
| 512Hz fixed update | Brain thread advances at a stable, known tick rate |
| Fixed32 math | `SimFloat = SimFloatImpl<Fixed32>` under `TNX_DETERMINISM` — integer arithmetic, no FP variability |
| Authoritative state in slab | All simulation state lives in contiguous, trivially-copyable field arrays |
| Deferred destruction on Logic thread | All entity destroy finalization is serialized through the Brain thread |
| Defrag disabled in determinism builds | `EntityCacheIndex` never changes for live authoritative entities |
| `JPH_CROSS_PLATFORM_DETERMINISTIC` | Jolt disables FMA and forces precise FP — required for consistent rollback |

---

## `TNX_DETERMINISM` Build Flag

Enabling `TNX_DETERMINISM`:
- Switches `SimFloat` from `SimFloatImpl<float>` to `SimFloatImpl<Fixed32>`
- Enables deterministic deferred-destruction ordering
- Disables defrag for live authoritative entities

`TNX_ENABLE_ROLLBACK` and `TNX_DETERMINISM` are **independent**:

| Flag | Enables |
|---|---|
| `TNX_ENABLE_ROLLBACK` | Temporal ring buffer storage (N-frame history), Jolt snapshots, the `RollbackSim` system |
| `TNX_DETERMINISM` | Fixed32 `SimFloat`, `/fp:precise` / `-ffp-contract=off`, `JPH_CROSS_PLATFORM_DETERMINISTIC` on Jolt |

Rollback without determinism re-simulates reproducibly **on the same machine and binary** — enough for server-side
reconciliation and editor history. Bit-identical results **across machines** (lockstep peers, cross-platform replays)
require `TNX_DETERMINISM`. Editor builds always enable rollback (the PIE server reconciles with it).

---

## `EntityCacheIndex` Stability

The entire engine uses a single flat `EntityCacheIndex` as the column coordinate across all tiers. Any operation that moves an entity changes its index and therefore its identity in the slab.

### Determinism Build Policy

- **Defrag of live authoritative entities: disabled.** Moving entities would change `EntityCacheIndex`, breaking rollback — two runs with the same inputs would produce different cache indices and diverge.
- **Slot reuse after deterministic destruction: allowed.** Tombstoned slots can be refilled; this doesn't change existing indices.
- **Chunk mirror storage:** The slab allocator uses a free-range list to reclaim freed space from destroyed chunks without moving live entities.

### Non-Determinism Builds / Editor

- Entity slot defrag may be enabled to improve locality
- Slab segment defrag may run as an optional background job
- Views rebind via `EntityIDChanged()` / `OnEntityMoved` listener subscription when defrag fires

---

## Deferred Destruction — Deterministic Order

Finalization of deferred destruction is the sole responsibility of the Logic thread.

In determinism builds:
- Apply finalize-destroy at a deterministic phase boundary (top of the Brain frame)
- Process pending destroys in ascending `EntityCacheIndex` order (or stable request order)
- Feed freed slots into a deterministic freelist policy

This ensures free order is not affected by thread scheduling or timing jitter — two runs with identical inputs produce identical slot allocations.

---

## Jolt Determinism

Jolt's `SaveState` / `RestoreState` via `StateRecorderImpl` is the foundation of rollback. It preserves the exact contact cache, solver warmstarting, and sleep states.

**Why not rebuild from slab positions:** A cold solver restart diverges from the original timeline because constraint forces are re-initialized from scratch rather than continuing from the previous warm solution. Snapshot restore is the only correct approach. See [Rollback Netcode](../networking/Rollback-Netcode.md).

**Per-frame snapshot overhead:** ~7KB per physics frame for 56 bodies. Saved after each `PullActiveTransforms`. Restore time <1ms.

---

## Empirical Validation (2026-03-29)

Rollback determinism verified via `ExecuteRollbackTest` with `TNX_ENABLE_ROLLBACK=ON`, `TNX_TESTING=ON`:

| Component | Result | Data Size | Notes |
|---|---|---|---|
| ECS temporal slab | Byte-perfect | 34 MB | All field data, 100K entities |
| Jolt physics | Byte-perfect | 7,436 bytes | Full physics state (contacts, solver, sleep) |

**Test conditions:** 100K entities (supercubes) + 56 physics bodies (5-layer pyramid), 512Hz fixed step, PhysDivisor=8 (64Hz physics), 5–12 frame rollbacks aligned to Flush+Pull boundaries.

**Resim performance:** ~18ms for 5 frames of full-frame resimulation (no dirty propagation). Dirty propagation — resimulating only corrected entities — is designed but not yet wired; expected to reduce resim cost by 10–100× for typical corrections affecting <1% of entities.

---

## `MetaRegistry` — Planned Design

> **Status: Design intent — not yet implemented.**

The `MetaRegistry` concept is a baked asset representing the engine's semantic truth: component list, tier, field layout, semantic indices. It would be checked into source control and validated at build time.

**Purpose:** Remove reliance on static initializer ordering (currently fragile — see Known Issues).

**Planned workflow:**
1. Static registrars (`TNX_TEMPORAL_FIELDS`, etc.) emit registration records during development
2. A precompile step bakes the `MetaRegistry` asset
3. At runtime, the engine loads the baked registry and finalizes against it
4. If static registrars don't match the baked registry: fail loudly

Until this is implemented, the engine relies on static constructors resolving before `TrinyxEngine::Initialize()`. This works in practice but is fragile across TUs.

---

## Network Contract

The networking layer iterates slab-backed authoritative state directly. If a value must replicate, it must live in slab-backed component data.

Constructs are orchestration objects and are not inherently networked unless they drive slab state.

**Delta serialization (planned default):** Send only fields/components that changed relative to a baseline. Pairs naturally with the existing dirty-bit tracking infrastructure.

---

## Files

| File | Purpose |
|---|---|
| `src/Runtime/Math/Public/Fixed32.h` | `Fixed32` scalar — int32, 0.1mm precision |
| `src/Runtime/Math/Public/SimFloat.h` | `SimFloat` alias — toggles Fixed32 vs float via `TNX_DETERMINISM` |
| `src/Runtime/Core/Public/TemporalComponentCache.h` | N-frame ring buffer; rollback-aware read/write |
| `src/Runtime/Core/Public/LogicThread.h` | `LogicThread<TNet, TRollback, TFrame>` — `TRollback` axis controls rollback behavior |
