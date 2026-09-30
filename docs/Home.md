# TrinyxEngine Wiki

TrinyxEngine is a C++20, data-oriented game engine built for competitive multiplayer games where **determinism, rollback netcode, and input latency are first-class design constraints**. It targets 100,000+ dynamic entities at 512Hz fixed update (1.95ms/frame budget) while exposing a familiar OOP-style API to gameplay authors.


---

## Navigation

### Getting Started
| Page | Description |
|---|---|
| [Installation](getting-started/Installation.md) | Clone, build, and run the testbed |
| [Build Options](getting-started/Build-Options.md) | All CMake flags and common build configurations |
| [Configuration](getting-started/Configuration.md) | EngineConfig reference and presets |

### Architecture
| Page | Description |
|---|---|
| [Overview](architecture/Overview.md) | Design philosophy and the three load-bearing decisions |
| [Threading Model](architecture/Threading.md) | Sentinel/Brain/Encoder trinity + job system |
| [ECS & Storage](architecture/ECS-And-Storage.md) | Tiered SoA storage, partition layout, the "spreadsheet" model |
| [Component System](architecture/Component-System.md) | FieldProxy, registration macros, schema validation |
| [Entity Lifecycle](architecture/Entity-Lifecycle.md) | Spawn, despawn, handle spaces, defragmentation |

### Gameplay Layer
| Page | Description |
|---|---|
| [Constructs & Views](gameplay/Constructs-And-Views.md) | `Construct<T>`, `Owned<T>`, `ConstructView<TEntity>`, tick dispatch |
| [Game Flow](gameplay/Game-Flow.md) | FlowManager, FlowState, GameMode, Soul/Body pattern, travel |
| [Physics](gameplay/Physics.md) | Jolt integration, JoltCharacter, constraint system |
| [Audio](gameplay/Audio.md) | AudioManager, voice pool, GPU Physics-Based Audio roadmap |

### Networking
| Page | Description |
|---|---|
| [Overview](networking/Overview.md) | Vocabulary, GNS, authority model, PIE loopback |
| [Connection Flow](networking/Connection-Flow.md) | Handshake → clock sync → level load → spawn (4 phases) |
| [Entity Replication](networking/Entity-Replication.md) | ServerClientChannel, spawn replication, state corrections |
| [Rollback Netcode](networking/Rollback-Netcode.md) | Rollback design, Jolt snapshots, resimulation |
| [Despawn Protocol](networking/Despawn-Protocol.md) | Four-phase networked entity despawn |

### Rendering
| Page | Description |
|---|---|
| [Overview](rendering/Overview.md) | VizBuffer architecture, current state vs roadmap |
| [GPU Compute Pipeline](rendering/GPU-Pipeline.md) | Predicate → prefix_sum → scatter → build_draws → sort_instances, GPU skinning, Buffer Device Address |
| [Dirty-Bit Upload](rendering/Dirty-Bit-Upload.md) | Selective GPU upload, 5 InstanceBuffers, SIMD OR path |

### Editor
| Page | Description |
|---|---|
| [Overview](editor/Overview.md) | 8 panels, PIE, undo/redo, asset database |
| [Debugging & Tooling](editor/Debugging.md) | Slab heatmap, spatial debug, profiling suite |

### Math & Determinism
| Page | Description |
|---|---|
| [Fixed-Point Math](math-and-determinism/Fixed-Point.md) | Fixed32, coordinate system, SimFloat alias |
| [Determinism Mode](math-and-determinism/Determinism.md) | Build options, MetaRegistry, validation harness |

### Reference
| Page | Description |
|---|---|
| [Performance Targets](reference/Performance-Targets.md) | Benchmarks, budgets, scalability targets |
| [Status & Roadmap](reference/Status-And-Roadmap.md) | Current milestone status and upcoming work |
| [Design Decisions](reference/Design-Decisions.md) | Why key architectural decisions were made |
| [Code Structure](reference/Code-Structure.md) | Headers declare, sources define — where implementations live and why |
| [Known Issues](reference/Known-Issues.md) | Current technical debt and known bugs |
| [Schema Error Reference](reference/Schema-Error-Reference.md) | Component validation error messages |

---

## Engine at a Glance

```
┌─────────────────────────────────────────────────────────────────┐
│  Sentinel (1000Hz)  │  Brain (512Hz)  │  Encoder (variable)    │
│  Input polling      │  Fixed logic    │  GPU upload + render    │
│  Vulkan lifetime    │  Job dispatch   │  Job dispatch           │
└─────────────────────────────────────────────────────────────────┘
           │                  │                    │
           └──────────────────┴────────────────────┘
                        Worker Pool (N-3 cores)
                  Logic Queue / Render Queue / Physics Queue

┌─────────────────────────────────────────────────────────────────┐
│  Gameplay Layer                                                  │
│  Construct<T> (singular OOP)  |  Entity (SoA horde, SIMD swept) │
│  Owned<T> composition         |  TNX_REGISTER_ENTITY            │
│  ConstructView<TEntity>       |  PrePhysics(dt), PostPhysics(dt) │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  ECS Storage Tiers                                               │
│  Cold (chunks, AoS)  │  Volatile (3-frame SoA)                  │
│  Static (read-only)  │  Temporal (N-frame rollback ring)        │
└─────────────────────────────────────────────────────────────────┘
```

## Three Design Constraints

The entire architecture derives from three interlocking decisions:

1. **Fixed 512Hz logic update.** Enables deterministic rollback, precise input timestamps, and consistent physics integration. Not configurable during a session — everything downstream assumes a known tick rate.

2. **Tiered SoA storage with rollback as a first-class citizen.** Hot component data lives in SoA ring buffers (Temporal: N-frame rollback, Volatile: triple-buffer). Cold data lives in archetype chunks. The tier is declared per-component, not per-entity.

3. **OOP API over a data-oriented substrate.** `Construct<T>` gives gameplay authors familiar OOP patterns. `ConstructView<TEntity>` decomposes them into SoA field arrays transparently. Gameplay authors write `transform.PosX += vel.VelX * dt`; the engine sweeps it 8-wide with AVX2.

---

## Quick Start

```bash
# Clone with submodules
git clone --recursive https://github.com/X64Tyko/TrinyxEngine.git
cd TrinyxEngine

# Build (RelWithDebInfo recommended for profiling)
cmake -B cmake-build-relwithdebinfo -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build cmake-build-relwithdebinfo

# Run the testbed
./cmake-build-relwithdebinfo/Testbed/Testbed
```

See [Installation](getting-started/Installation.md) for full setup, IDE integration, and troubleshooting.

---

## Status

**Current phase (2026-09):** Foundation Stage — Animation and networking in progress

| Stage | Milestone | Status |
|---|---|---|
| Foundation | Editor | Complete |
| Foundation | Construct/View OOP | Complete |
| Foundation | Networking | In Progress |
| Foundation | Audio | Complete |
| Foundation | Camera System | Complete |
| Foundation | Game Flow | In Progress |
| Foundation | Animation | In Progress |
| Hardening | Hot-path audit, constraint system, static tier | Planned |

Full details: [Status & Roadmap](reference/Status-And-Roadmap.md)
