# Defragmentation (TrinyxEngine)

This document describes how defragmentation/compaction is intended to work and how it interacts with
Entity identity, Views, and determinism builds.

## Why Defrag Exists

Defrag/compaction can improve locality and reduce fragmentation in slab/chunk allocators.
However, TrinyxEngine uses `EntityCacheIndex` as a globally valid identity across tiers, which makes
moving entities inherently risky for deterministic simulation.

Therefore, defrag must be:

- optional
- configurable
- explicitly disabled in determinism builds for authoritative data

---

## Core Constraint: EntityCacheIndex

`EntityCacheIndex` is valid across tiers and treated as identity.

Any defrag system that moves live entities between slots changes `EntityCacheIndex` and therefore changes identity.

---

## Two Kinds of Defrag

TrinyxEngine has two distinct kinds of defragmentation, and **both can change identity** depending on build mode.

### Background: `EntityCacheIndex` is a column index (identity)

`EntityCacheIndex` is consistent across all tiers. Conceptually, treat slab-backed storage as a single
spreadsheet:

- **Rows** = fields (including fields from Volatile, Temporal, and cold chunk mirrors)
- **Columns** = entities (`EntityCacheIndex`)

Volatile and Temporal are not separate “entity spaces” — they are different row ranges within the same
global spreadsheet. A chunk allocation claims a contiguous range of columns based on its
`EntitiesPerChunk`. A chunk’s cold/mirror fields occupy additional rows but still index into the same
columns.

**Consequence:** any operation that relocates a chunk’s claimed column range changes `EntityCacheIndex`
for every entity in that chunk, across all tiers.

---

### 1) Entity slot defrag (within the chunk)

Moves live entities between columns within the chunk space inside the global spreadsheet.

This changes `EntityCacheIndex` and therefore changes identity. Views must rehydrate and all systems
holding indices/handles must be updated.

---

### 2) Chunk mirror relocation (chunk-level compaction)

Relocates a chunk’s contiguous column range (and its associated cold/mirror row blocks) to close holes
in the global spreadsheet.

This also changes `EntityCacheIndex` for every entity in the moved chunk, because `EntityCacheIndex`
is the column index.

Chunk mirror relocation therefore requires the same identity-change notifications and rehydration
behavior as entity slot defrag, but performed at chunk granularity.

---

## Determinism Builds

In determinism builds:

- entity slot defrag/compaction that moves live authoritative entities is disabled
- slot reuse after deterministic destruction is allowed
- the system is tuned to avoid defrag spikes at large rollback depths (e.g. 128+ frames)

### Slab mirror reuse is still required

Even with entity slot defrag disabled, slab allocators must be able to reuse freed space from chunk mirrors.

When an archetype chunk is destroyed, its mirrored storage regions (Temporal/Volatile/Static) must be released back to
a free-space structure so future chunks can reuse that space. This prevents the "4 arena heads" bump allocation model
from leaking memory over time.

---

## Non-Deterministic Builds / Editor Sessions

In non-deterministic builds or editor sessions:

- entity slot defrag may be enabled to improve locality (optional)
- slab segment defrag may be enabled to compact chunk mirror storage (optional)

When defrag moves entities, the engine must:

- emit an identity-change notification (e.g. `EntityIDChanged()` / `EntityIndexChanged()`)
- ensure all Views rebind their FieldProxy cursors

When slab segment defrag moves chunk mirror blocks, the engine must:

- update each affected chunk’s slab offsets (TemporalOffset / VolatileOffset / StaticOffset)
- ensure no thread observes partially-moved data (must run at a safe point)
- Perform the same identity-change notifications and rebinds for the moved chunk and its entities.

---

## View Rehydration Contract

Views are CRTP lenses into ECS data. They hold cursors into SoA field arrays (FieldProxy bindings).

When the allocator moves entity data:

- Views must rebind their FieldProxies
- Constructs should not need to manually repair pointers

A typical hook is:

- `EntityIDChanged()` or `OnEntityMoved(oldIndex, newIndex)`

The defrag system is responsible for firing this hook for any subscribed View/Construct.

Note: slab segment defrag (chunk mirror compaction) does not inherently require `EntityIDChanged()` because entity
indices do not change. However, any system caching raw slab pointers for a chunk’s mirror region must refresh them
after compaction.

---

## Slab Segment Allocator (Chunk Mirror Storage)

Current slab allocators effectively have a bump `head` per arena region (e.g., 4 heads), tracking total space used for
chunk mirror storage. When chunks are destroyed, the engine must reclaim their mirror blocks.

### Free-space tracking (required in all modes)

Each slab arena maintains a set of free ranges:

- `[{offset, size}]` ranges within the slab mirror address space
- insertion on free
- coalescing of adjacent ranges
- deterministic selection policy when allocating (especially in determinism builds)

Allocation strategy:

- try to satisfy the request from the free-range list (first-fit / best-fit policy)
- if no free range fits, allocate from the bump `head`

Free strategy:

- push freed range into the free-range list
- coalesce neighbors to avoid fragmentation

### Determinism requirements

In determinism builds:

- allocation decisions must be deterministic (e.g., lowest-offset first-fit with deterministic tie-breaks)
- frees must be finalized in deterministic order (Logic thread finalize pass) so the free-range state is stable
- compaction that moves live chunk mirrors may be disabled, but reuse of holes must work

### Optional compaction job (non-deterministic)

In non-deterministic builds, an additional compaction job may run to reduce fragmentation:

- build a list of live chunk mirror segments in address order
- move segments toward the front to eliminate holes
- update per-chunk slab offsets to new locations
- rebuild the free-range list and set the bump head accordingly

This compaction moves *chunk mirror blocks*, not singular entities. `EntityCacheIndex` is still changed.

---

## Safety / Debugging

When defrag is enabled:

- the engine should have debug modes that validate:
    - no stale cursors exist (optional expensive checks)
    - defrag does not occur during unsafe windows (mid-frame iteration)
    - defrag events are ordered and processed on the Logic thread handshake boundary

Safe point recommendation:

- run defrag/compaction only at a defined barrier (e.g., top of Brain frame after joining jobs) so no worker is reading
  or writing slab memory while it is being modified.

---

## TODO: Implementation Notes

- Define exact “safe points” where defrag may run (likely at the top of the Brain frame)
- Define subscription model for listeners (Views, Registry, editor selection handles)
- Define logging/telemetry for moved entities
- Define deterministic free-list policy and document it (first-fit vs best-fit, ordering, tie breaks)