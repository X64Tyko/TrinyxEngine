# Determinism (TrinyxEngine)

This document defines what "deterministic simulation" means in TrinyxEngine and what rules/constraints
exist to keep rollback, replay, and networking stable.

## Scope

Determinism applies to **authoritative simulation state**:

- entity component data in slab-backed tiers (Temporal / Volatile / Static / Cold where applicable)
- allocation/free behavior that affects `EntityCacheIndex` stability
- any gameplay-affecting math in fixed update

Determinism does NOT apply to:

- rendering outputs (GPU results)
- editor-only state and transient UI state
- performance timing and scheduling (unless it changes sim outcomes)

---

## Core Idea

TrinyxEngine is designed so determinism is primarily enforced by **architecture**, not by developer discipline.

Key mechanisms:

- fixed update (Brain thread) at a stable tick rate
- authoritative state lives in slab-backed component data
- deferred destruction finalization is owned by the Logic thread and can be made deterministic
- build options allow enabling/disabling defrag and Fixed32 math

---

## MetaRegistry and Deterministic Identity

### MetaRegistry is an asset

`MetaRegistry` is a baked asset that represents the engine’s semantic truth:

- component list
- component tier (Temporal/Volatile/Static/Cold)
- per-component field layout and ordering
- field semantic indices and offsets

**Workflow contract:** the baked `MetaRegistry` asset is checked into source control like any other asset.
If the baked MetaRegistry changes unexpectedly, the build/editor must be loud about it.

### Loud validation

The engine/editor should warn/fail loudly when:

- MetaRegistry asset has changed unexpectedly
- a deterministic build is running without a baked registry
- runtime static registration does not match baked registry expectations (if enabled)

### Static registration vs baked registry

Static registration is useful for development iteration. In deterministic builds it may be disabled.

Implementation model:

- static registrars emit registration records
- at startup, engine loads baked MetaRegistry and finalizes the runtime registry from it
- if static registrars are enabled, they may be used to validate or append (dev-only)

---

## EntityCacheIndex Stability

`EntityCacheIndex` is valid across tiers. Any operation that changes an entity’s index changes its identity.

### Deterministic build policy

In determinism builds:

- the engine must avoid moving live entities between slots (`EntityCacheIndex` changes)
- defragmentation/compaction is disabled
- slot reuse is allowed only through a deterministic destruction/freelist policy

This avoids defrag spikes for large rollback depths (e.g. 128+ frames) at the cost of acceptable fragmentation within
chunks.

---

## Deferred Destruction and Deterministic Free Order

Finalization of deferred destruction is the sole responsibility of the Logic thread.

In determinism builds, destruction finalization should normalize ordering, e.g.:

- apply finalize-destroy in a deterministic phase boundary (top of Brain frame)
- process pending destroys in ascending `EntityCacheIndex` (or stable request order)
- feed freed slots into a deterministic free list policy

This ensures that free order is not affected by thread scheduling or timing jitter.

---

## Numeric Determinism

Simulation code should use the engine’s canonical numeric alias types:

- `SimFloat` (may map to `float` or `Fixed32` depending on build options)

Deterministic builds may disable float-based simulation paths entirely by enabling Fixed32.

---

## World Coordinates and Cells

Transforms are cell-local.

Even without streaming, the cell coordinate system enables very large worlds (~45M km target bound)
while keeping simulation numeric stability and minimizing float error (for Jolt simulation particularly).

---

## Networking Contract (High Level)

The networking layer iterates slab-backed authoritative state directly.
If a value must replicate, it must live in slab-backed component data.

Constructs are orchestration objects and are not inherently networked unless they drive slab state.

### Component-Level Serialization (planned)

Networking is component-driven. Components may optionally define their own net serialization behavior.

This enables studios to:

- define a custom `Transform` component with bespoke quantization rules
- implement per-component relevancy / `ShouldSerialize` logic
- implement specialized packing (bitpacking, bounds-aware compression, etc.)

This is conceptually similar to Unreal's `NetSerialize` / `NetDeltaSerialize`, but intended to be
simpler and more composable.

### Delta Serialization (planned default)

The engine intends to default to delta serialization:

- send only fields/components that changed relative to a chosen baseline
- baseline may be last-acked state, last snapshot, or deterministic rollback frame state

Delta serialization pairs naturally with existing dirty-bit tracking and is expected to be the default
behavior in most networked games.

---

## Build Options (to be kept in sync with docs/BUILD_OPTIONS.md)

- Determinism mode (enables deterministic policies; optionally disables defrag/compaction)
- Fixed32 enable/disable (`SimFloat` mapping)
- Static registration enable/disable (dev iteration vs baked-only startup)

(TODO: link exact CMake options once finalized)