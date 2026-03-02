# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

---

## Project Overview

**TrinyxEngine** is a C++20, data-oriented game engine R&D project. The goal is to run 100,000+ dynamic entities at 512Hz fixed update (1.95ms/frame budget) while exposing an OOP-style API to entity authors. It is structured as two CMake targets: `TrinyxEngine` (static library) and `Testbed` (executable).

---

## Build Commands

**Requirements (Linux):** CMake 3.20+, C++20 compiler, SDL3 system package (`libsdl3-dev`), Tracy submodule (`libs/tracy`).

```bash
# Standard development build (RelWithDebInfo recommended for profiling)
cmake -B cmake-build-relwithdebinfo -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build cmake-build-relwithdebinfo

# Run the testbed
./cmake-build-relwithdebinfo/Testbed/Testbed

# Debug build
cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug
```

**Key CMake options** (append to the configure step):
| Option | Default | Purpose |
|--------|---------|---------|
| `ENABLE_TRACY=ON/OFF` | ON | Tracy profiler integration |
| `TRACY_PROFILE_LEVEL=1/2/3` | 3 | 1=coarse (~1%), 2=medium (~5%), 3=per-entity (~50%+ overhead) |
| `ENABLE_AVX2=ON/OFF` | ON | `-march=native` on GCC/Clang |
| `GENERATE_ASSEMBLY=ON/OFF` | OFF | Emit `.s` files for vectorization inspection |
| `VECTORIZATION_REPORTS=ON/OFF` | OFF | Compiler loop-vectorization diagnostics |
| `TNX_ALIGN_64=ON/OFF` | OFF | 64-byte vs 32-byte field array alignment |

---

## Architecture

### Threading Model — The Trinyx Trinity

Three dedicated threads + a shared worker pool:

- **Sentinel (main thread):** 1000Hz input polling, GPU resource management, frame pacing.
- **Brain (logic thread):** 512Hz fixed-timestep coordinator. Submits `LogicQueue` jobs, then acts as a worker until jobs complete.
- **Encoder (render thread):** Variable-rate render coordinator. Submits `RenderQueue` jobs, then acts as a worker.
- **Worker pool:** The remaining cores pull from both queues (work-stealing).

Brain and Encoder are **coordinators, not dedicated workers**. On an 8-core CPU this gives ~4× speedup for both logic and render passes.

### Entity–Component System

The ECS is archetype-based. Entities of the same component signature share an **Archetype**, whose data lives in 64 KB **Chunks** (`src/Runtime/Memory/`).

Hot components (those marked `TNX_TEMPORAL_FIELDS`) are decomposed into Structure-of-Arrays and placed in the **TemporalComponentCache** (`TemporalComponentCache.h/cpp`). This is the current implementation of the dual-buffer (ReadArray/WriteArray) system that will eventually migrate to the full History Slab.

Cold components (no `TNX_TEMPORAL_FIELDS`) live only in Archetype chunks.

### FieldProxy — SoA with OOP Syntax

`FieldProxy<T, FieldWidth>` (`src/Runtime/Core/Public/FieldProxy.h`) is the core abstraction. Each field proxy holds:
- `ReadArray` — frame T (current simulation state, read-only during update)
- `WriteArray` — frame T+1 (next state, written during update)
- `index` — current entity offset within the SoA arrays
- `mask` — AVX2 mask for `WideMask` mode partial-lane writes

`FieldWidth` has three modes:
- `Scalar` — one entity per loop iteration (simple, safe)
- `Wide` — 8 entities per AVX2 instruction (maximum throughput)
- `WideMask` — like `Wide` but with a tail mask for non-multiple-of-8 counts

`Bind()` copies from ReadArray→WriteArray and caches the index. `Advance(step)` increments the index and propagates the copy. All arithmetic operators (`+=`, `-=`, `*=`, etc.) write to `WriteArray` and read from `WriteArray` for the accumulate path.

### Entity Definition Pattern

Entities are CRTP structs templated on `FieldWidth`:

```cpp
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct CubeEntity : EntityView<CubeEntity, WIDTH> {
    Transform<WIDTH> transform;
    Velocity<WIDTH>  velocity;
    ColorData<WIDTH> color;

    FORCE_INLINE void PrePhysics(double dt) {
        transform.PositionX += velocity.VelocityX * static_cast<float>(dt);
        // etc.
    }

    TNX_REGISTER_SCHEMA(CubeEntity, EntityView, transform, velocity, color)
};
TNX_REGISTER_ENTITY(CubeEntity)
```

Components with temporal (SoA) fields use:
```cpp
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct Transform {
    FieldProxy<float, WIDTH> PositionX, PositionY, PositionZ;
    // ...
    TNX_TEMPORAL_FIELDS(Transform, PositionX, PositionY, PositionZ, ...)
};
TNX_REGISTER_COMPONENT(Transform)
```

Cold (non-temporal) components use plain POD structs with `TNX_REGISTER_FIELDS` (no `TNX_TEMPORAL_FIELDS`).

### Reflection / Registration Macros

All macros are defined in `SchemaReflector.h` and `SchemaValidation.h`:

| Macro | Where used | Purpose |
|-------|-----------|---------|
| `TNX_REGISTER_ENTITY(T)` | After entity class | Triggers static registration via `PrefabReflector<T<>>::Register()` |
| `TNX_REGISTER_SCHEMA(Class, Super, ...)` | Inside entity class body | Generates `DefineSchema()`, `Advance()`, type aliases |
| `TNX_REGISTER_SUPER_SCHEMA(Class, Super, ...)` | Inside non-leaf base entity | Same as above but for intermediate template bases |
| `TNX_REGISTER_COMPONENT(T)` | After component struct | Registers field metadata via static initializer |
| `TNX_REGISTER_FIELDS(T, ...)` | Inside component | Generates `DefineFields()`, `FieldNames`, `Bind()`, `Advance()` |
| `TNX_TEMPORAL_FIELDS(T, ...)` | Inside component | Like `TNX_REGISTER_FIELDS` but sets `bTemporalComp = true` |


### Key Source Locations

| Path | Contents |
|------|----------|
| `src/Runtime/Core/Public/FieldProxy.h` | FieldProxy + SIMDTraits + FieldMask |
| `src/Runtime/Core/Public/Types.h` | EntityID, FieldWidth, InstanceData, math types |
| `src/Runtime/Core/Public/SchemaReflector.h` | All registration macros and PrefabReflector |
| `src/Runtime/Core/Public/EngineConfig.h` | EngineConfig struct (frequencies, entity limits, history depth) |
| `src/Runtime/Core/Public/EntityView.h` | CRTP base for entities |
| `src/Runtime/Memory/Public/Archetype.h` | Archetype (chunk layout, field array table) |
| `src/Runtime/Memory/Public/Registry.h` | Entity lifecycle (Create/Destroy/query) |
| `src/Runtime/Memory/Public/TemporalComponentCache.h` | Dual-buffer SoA storage (proto History Slab) |
| `src/Runtime/Rendering/Public/RenderThread.h` | Encoder thread |
| `src/Runtime/Core/Private/LogicThread.cpp` | Brain thread |
| `Testbed/src/Main.cpp` | Test suite entry point (uses `Trinyx::Testing`) |

### Planned but Not Yet Implemented

- **History Slab** — multi-frame arena allocator for zero-copy rendering, rollback netcode, replay. Design is in `docs/ARCHITECTURE.md`. Currently approximated by `TemporalComponentCache`.
- **State-sorted rendering** — 64-bit sort keys + radix sort (design in `docs/DATA_STRUCTURES.md`).
- **Jolt Physics** integration.
- **Job system** (jobs are currently run inline; infrastructure exists in Brain/Encoder threads).

---

## Hard Constraints (by design)

1. **No virtual functions** in entities or components — enforced at compile time by `TNX_REGISTER_ENTITY`.
2. **No `std::string`/`std::vector`** in components — enforced by `VALIDATE_COMPONENT_IS_POD`.
3. **No heap allocation in `PrePhysics`/`PostPhysics`/`Render`** — zero-frame-allocation budget.
4. **All inter-thread communication** via atomics/lock-free structures only.
5. **GPU calls only on the Encoder thread**.

---

## Docs

- `docs/ARCHITECTURE.md` — History Slab memory layout, thread access patterns
- `docs/DATA_STRUCTURES.md` — FieldProxy, EntityView, InstanceData, sort keys
- `docs/BUILD_OPTIONS.md` — CMake option details and common configurations
- `docs/SCHEMA_ERROR_EXAMPLES.md` — Reflection gotchas and compile/runtime errors
- `docs/CONFIGURATION.md` — EngineConfig presets
- `docs/GPU_Driven_Rendering_Design.md` — GPU-driven pipeline design doc