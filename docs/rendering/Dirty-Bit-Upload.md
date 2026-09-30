# Dirty Bit Upload

> [← GPU Pipeline](GPU-Pipeline.md) | [Home](../Home.md)

---

## Overview

The engine uploads only modified entities per frame rather than the full slab. A double-buffered cumulative dirty bit array (12.5KB for 100K entities — fits in L1) tracks which entities changed. The render thread reads this to build the minimal upload set.

**Status:** Operational since 2026-05. In steady state at 100K entities + 56 physics bodies, the render thread runs at ~0.88ms (1133 FPS) driven almost entirely by dirty-bit selective upload.

---

## Three Dirty Bits

All three bits live in the entity's flags field (`CacheSlotMeta`):

| Bit | Name | Semantics |
|---|---|---|
| Bit 31 | `Active` | Entity is alive and should be rendered / simulated |
| Bit 30 | `Dirty` | Cumulative: this entity has been modified since last render acknowledgment |
| Bit 29 | `DirtiedFrame` | Per-frame: set on any write this logic tick, cleared unconditionally at frame start |

**`DirtiedFrame` (bit 29):** Cleared at the top of every logic frame. Set by `FieldProxy::operator=` on any write. Used to compute the current frame's dirty set.

**`Dirty` (bit 30):** Accumulates across frames until the render thread acknowledges via `RenderAck` atomic handshake. If the render thread falls behind N logic frames, `Dirty` accumulates all N frames' worth of changes. When the render thread finally catches up, it SIMDs OR the dirty arrays from all intermediate frames into one upload set.

---

## `FieldProxy` Dirty Marking

Every `FieldProxy::operator=` marks both dirty bits atomically:

```cpp
template<typename T, FieldWidth WIDTH>
FieldProxy<T, WIDTH>& operator=(const T& value) {
    *ptr = value;
    flags |= TemporalFlagBits::Dirty | TemporalFlagBits::DirtiedFrame;
    return *this;
}
```

The flags array is at a known offset from the field array base — always `CurrFieldAddrs[0]` by convention (SemFlags = 1, index 0). There is no indirection.

---

## Per-Slab Bitplanes

There are 5 heap-allocated GPU dirty bitplanes — one per field slab slot. Each bitplane is a 12.5KB bit array (100K entities / 8 bits per byte).

**Logic side:** After the AVX2 entity sweep, the engine scans the slab flags array with AVX2, ORing the active+dirty bits into all 5 bitplanes that correspond to slabs currently writable by the render thread.

**`PullActiveTransforms` special case:** Jolt writes entity positions back into the slab after each physics step, bypassing `FieldProxy`. `PullActiveTransforms` manually marks dirty bits for all awake bodies after Jolt writeback.

**`FirstSlabWrite`:** The first write to each slab slot does a full memcpy to bootstrap GPU state, since there is no previous frame to delta against. Subsequent writes use dirty-bit selective upload.

---

## `RenderAck` Handshake

The render thread atomically acknowledges each frame's dirty bits after uploading:

1. Logic writes `DirtiedFrame` bits during the logic tick
2. `PublishCompletedFrame` copies `DirtiedFrame` → `Dirty` for the completed frame
3. Render thread reads `Dirty` bits for the frame it is about to upload
4. Render thread uploads only the dirty entities
5. Render thread writes `RenderAck` for that frame
6. On the next logic tick, `Dirty` bits acknowledged by `RenderAck` are cleared

If the render thread is processing frame T and the logic thread is already on frame T+3, the logic thread will OR frames T+1, T+2, T+3's dirty bits into the cumulative set before the render thread picks up.

---

## SIMD OR Path — Catching Up After Lag

When the render thread falls behind multiple logic frames, it cannot simply upload the most recent frame's data — entities modified in intermediate frames would be missed.

```
Logic frames: T  T+1  T+2  T+3 (current)
Render on:     T-1 (just finished)

→ Must upload changes from T, T+1, T+2, T+3

SIMD OR: dirty[T] | dirty[T+1] | dirty[T+2] | dirty[T+3]
→ One combined upload set, no full-slab copy
```

The 12.5KB bitplane (100K entities) fits in L1 cache. OR-ing 4 frames is 4 AVX2 passes of 1562 256-bit operations — negligible cost.

---

## Selective Scatter

After building the upload set, the scatter pass iterates only entities with set bits:

```cpp
for (uint64_t word : dirty_bitplane) {
    while (word) {
        int bit = ctzll(word);          // find lowest set bit
        uint32_t entityIdx = base + bit;
        UploadEntity(entityIdx);
        word &= word - 1;               // clear lowest set bit
    }
}
```

`ctzll` (count trailing zeros) is a single CPU instruction. For a typical frame where 1% of entities move, this processes ~1000 entities instead of 100K.

---

## Dirty Bits and Rollback

The dirty set from a rollback correction propagates naturally through the resimulation: any entity that gets corrected writes via `FieldProxy`, which sets `Dirty`. After the resimulation, the upload set contains exactly the entities that changed during rollback — no manual dependency tracking required.

The same `Dirty` bit (bit 30) drives both GPU upload and rollback blast radius.

---

## `TemporalFlagBits` Enum

All entity flags live in one place:

```cpp
enum class TemporalFlagBits : int32_t
{
    Active           = 1u << 31, // entity ticks, renders, and simulates
    Dirty            = 1u << 30, // cumulative: needs GPU upload until render acknowledges
    DirtiedFrame     = 1u << 29, // per-frame: written this tick
    Replicated       = 1u << 28, // net: this entity replicates
    Alive            = 1u << 27, // data valid, slot cannot be reclaimed
    // bit 26 — unused (proposed: Discontinuity)
    NetConfirmedDead = 1u << 25, // despawn confirmed by Authority
    // bits [24:21] — 4-bit attachment depth (depthMask)
    PrePhysSkip      = 1u << 20, // skip PrePhysics sweep for this entity
    PostPhysSkip     = 1u << 19, // skip PostPhysics sweep for this entity
    ScalarSkip       = 1u << 18, // skip ScalarUpdate sweep for this entity
    ASleep           = 1u << 17, // physics body is sleeping
    Tombstone        = 1u << 16, // deferred destruction pending
    // bits [15:0] — available for game-layer flags
};
```

Typed `|=` / `&=` operators on `CacheSlotMeta` prevent raw integer mixing.
