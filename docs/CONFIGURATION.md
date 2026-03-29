# Engine Configuration

> **Navigation:** [← Back to README](../README.md) | [← Data Structures](DATA_STRUCTURES.md) | [Current Status →](STATUS.md)

---

## EngineConfig Structure

All engine timing, threading, and memory parameters are configured through `EngineConfig`
(`src/Runtime/Core/Public/EngineConfig.h`):

```cpp
struct EngineConfig
{
    // === Timing Configuration ===

    // Render rate cap. 0 = uncapped. Cannot exceed FixedUpdateHz when set.
    int TargetFPS = 0;

    // Fixed Update Rate (Logic / Physics coordinator)
    // Runs at deterministic timestep regardless of render frame rate.
    // Common values: 60, 128, 256, 512
    int FixedUpdateHz = 128;

    // Physics subdivisions per logic tick.
    // PhysicsHz = FixedUpdateHz / PhysicsUpdateInterval
    // e.g. FixedUpdateHz=512, PhysicsUpdateInterval=8 → 64Hz physics
    int PhysicsUpdateInterval = 8;

    // Network Tick Rate
    // How often to send/receive network packets.
    int NetworkUpdateHz = 30;

    // Input Polling Rate (Main Thread / Sentinel)
    // How often Sentinel samples SDL input events.
    int InputPollHz = 1000;

    // === Entity Budget Configuration ===
    //
    // Two fixed-size arenas, each with a dual-ended allocator:
    //
    //   Arena 1: Renderable  [0 .. MAX_RENDERABLE_ENTITIES)
    //     RENDER (→) from 0              — render-only (particles, decals)
    //     DUAL   (←) from MAX_RENDERABLE — physics + render (players, props)
    //
    //   Arena 2: Cached  [MAX_RENDERABLE_ENTITIES .. MAX_CACHED_ENTITIES)
    //     PHYS  (→) from MAX_RENDERABLE  — physics-only bodies, triggers
    //     LOGIC (←) from MAX_CACHED      — logic/rollback-only entities
    //
    // Constraint (validated at startup):
    //   MAX_RENDERABLE_ENTITIES <= MAX_CACHED_ENTITIES

    // Total size of Arena 1 (Renderable). RENDER + DUAL must fit within this.
    int MAX_RENDERABLE_ENTITIES = 11000;

    // Maximum Jolt physics bodies (sizes Jolt body arrays, separate from arena boundary).
    int MAX_JOLT_BODIES = 8000;

    // Total size of both arenas combined.
    int MAX_CACHED_ENTITIES = 25000;

    // === History / Frame Depth ===

    // Number of frames stored in the Temporal tier ring buffer.
    // MUST be power of 2, minimum 8.
    // Determines rollback depth: TemporalFrameCount / FixedUpdateHz = seconds of history.
    //
    // Examples @ 512Hz:
    //   8:   ~15.6ms  — minimum, just enough for render + one rollback step
    //   16:  ~31.2ms  — client prediction window
    //   64:  ~125ms   — lag compensation, replay
    //   128: ~250ms   — GGPO-style full rollback
    //
    // When TNX_ENABLE_ROLLBACK is not defined, Temporal entities fall back to 3-frame
    // triple-buffer (same as Volatile), saving significant memory.
    int TemporalFrameCount = 8;

    // Job queue pre-allocation size. Exceeding this limit will assert.
    int JobCacheSize = 16 * 1024;

    // === Helper Functions ===

    // Returns 0.0 when TargetFPS is 0 (uncapped).
    double GetTargetFrameTime() const;
    double GetFixedStepTime() const { return 1.0 / FixedUpdateHz; }
    double GetNetworkStepTime() const { return (NetworkUpdateHz > 0) ? 1.0 / NetworkUpdateHz : 0.0; }
};
```

**Note on Volatile frame count:** The Volatile slab is always a 3-frame triple-buffer (hardcoded in
`ComponentCache<CacheTier::Volatile>::GetFrameCount`). One frame is being written by Logic, one is
being read by the Encoder, and one is free. This is not configurable.

**Note on GPU instance buffers:** Five `PersistentMapped` GPU InstanceBuffers are maintained in flight
by `VulkRender`. This count is hardcoded to decouple the VSync clock from the logic thread's slab
locks and is not exposed through `EngineConfig`.



## Partition Size Rules

The two-arena layout must satisfy:

```
MaxRenderEntities + MaxDualEntities <= MaxRenderableEntities   // both renderable buckets fit in Arena 1
MaxRenderableEntities + MaxPhysEntities <= MaxCachedEntities   // arenas fit in total budget
```

Logic entities fill the remainder of Arena 2:
```
MaxLogicEntities = MAX_CACHED_ENTITIES - MAX_RENDERABLE_ENTITIES - (phys entities allocated so far)
```

**Physics iterates DUAL + PHYS at boundary:** `DUAL[MAX_RENDERABLE-N_dual..MAX_RENDERABLE)` +
`PHYS[MAX_RENDERABLE..MAX_RENDERABLE+N_phys)` + `STATIC`
**Render iterates RENDER + DUAL in Arena 1:** `RENDER[0..N_render)` + `DUAL[MAX_RENDERABLE-N_dual..MAX_RENDERABLE)` +
`STATIC`

Oversizing a partition or arena wastes memory but has no correctness impact. Undersizing causes a startup assertion
failure.

---

## Configuration Presets

### Single-Player / RPG

```cpp
EngineConfig rpgConfig;
rpgConfig.FixedUpdateHz        = 60;
rpgConfig.PhysicsUpdateInterval = 4;     // 15Hz physics
rpgConfig.MAX_RENDERABLE_ENTITIES = 10000;
rpgConfig.MAX_CACHED_ENTITIES  = 50000;
rpgConfig.TemporalFrameCount   = 8;
rpgConfig.NetworkUpdateHz      = 0;
```

**Use Case:** Single-player RPGs, narrative games, offline simulations.

---

### Lightweight (Rendering Focus)

```cpp
EngineConfig lightweightConfig;
lightweightConfig.FixedUpdateHz        = 60;
lightweightConfig.MAX_RENDERABLE_ENTITIES = 5000;
lightweightConfig.MAX_CACHED_ENTITIES  = 25000;
lightweightConfig.TemporalFrameCount   = 8;
lightweightConfig.NetworkUpdateHz      = 0;
```

**Use Case:** Single-player games, simple simulations, no networking.

---

### Balanced (Standard Game)

```cpp
EngineConfig balancedConfig;
balancedConfig.FixedUpdateHz        = 128;
balancedConfig.PhysicsUpdateInterval = 2;   // 64Hz physics
balancedConfig.MAX_RENDERABLE_ENTITIES = 25000;
balancedConfig.MAX_CACHED_ENTITIES  = 100000;
balancedConfig.TemporalFrameCount   = 16;   // ~125ms history @ 128Hz
balancedConfig.NetworkUpdateHz      = 30;
```

**Use Case:** Most networked games, action games.

---

### Competitive (High-Frequency)

```cpp
EngineConfig competitiveConfig;
competitiveConfig.FixedUpdateHz        = 512;
competitiveConfig.PhysicsUpdateInterval = 8;   // 64Hz physics
competitiveConfig.InputPollHz          = 1000;
competitiveConfig.MAX_RENDERABLE_ENTITIES = 15000;
competitiveConfig.MAX_CACHED_ENTITIES  = 25000;
competitiveConfig.TemporalFrameCount   = 128;  // ~250ms history @ 512Hz
competitiveConfig.NetworkUpdateHz      = 60;
```

**Use Case:** Fighting games, competitive shooters, esports titles.
Rollback depth = 250ms — enough for GGPO-style netcode at typical internet latency.
Requires `TNX_ENABLE_ROLLBACK` defined; without it, Temporal falls back to 3-frame triple-buffer.

---

### Simulation (Maximum Entities)

```cpp
EngineConfig simulationConfig;
simulationConfig.FixedUpdateHz        = 60;
simulationConfig.MAX_RENDERABLE_ENTITIES = 50000;
simulationConfig.MAX_CACHED_ENTITIES  = 250000;
simulationConfig.TemporalFrameCount   = 8;
simulationConfig.NetworkUpdateHz      = 0;
```

**Use Case:** Strategy games, crowd simulations. Lower Hz compensates for higher entity count.

---

## Memory Calculations

### Per-Entity Hot Data Size

| Component | Fields | Bytes/entity/frame |
|-----------|--------|--------------------|
| Transform | 9 floats (pos/rot/scale) | 36 B |
| Velocity  | 3 floats | 12 B |
| RigidBody | 6 floats (vel + angular vel) | 24 B |
| Material  | 4 floats (RGBA) | 16 B |
| TemporalFlags | 1 int32 (universal strip) | 4 B |
| **Typical dual entity** | | **~92 B** |

### Volatile Slab Size

```
VolatileSlab = MAX_CACHED_ENTITIES × HotBytesPerEntity × VolatileFrameCount (3)
             = 100,000 × 92B × 3 ≈ 27.6 MB
```


### Temporal Slab Size

```
TemporalSlab = MAX_CACHED_ENTITIES × HotBytesPerEntity × TemporalFrameCount
             = 100,000 × 92B × 8  ≈ 73.6 MB   (TemporalFrameCount = 8)
             = 100,000 × 92B × 128 ≈ 1.18 GB  (TemporalFrameCount = 128)
```

### Memory Table (MAX_CACHED_ENTITIES = 100k, 92 B/entity/frame)

| TemporalFrameCount | Temporal Slab | + Volatile (3) | Total Hot RAM |
|-------------------|--------------|----------------|---------------|
| 8 | 73.6 MB | 27.6 MB | ~101 MB |
| 16 | 147 MB | 27.6 MB | ~175 MB |
| 64 | 589 MB | 27.6 MB | ~617 MB |
| 128 | 1.18 GB | 27.6 MB | ~1.21 GB |

Add ~100 MB for cold components in archetype chunks + ECS metadata.

### GPU Memory

| Buffer | Size (100k entities) |
|--------|----------------------|
| InstanceBuffer × 5 | 5 × (MAX_CACHED_ENTITIES × FieldStride) ≈ 5 × 12 MB = 60 MB |
| Static Buffer | Static entity count × FieldStride ≈ few MB |
| Cumulative Dirty Bit Array | MAX_CACHED_ENTITIES / 8 × 2 (double-buffered) ≈ 25 KB |

---

## Performance Impact of Config

### FixedUpdateHz

| Hz | Frame Budget | Use Case |
|----|--------------|----------|
| 60 | 16.67ms | Standard games, VR |
| 128 | 7.81ms | Responsive action games |
| 256 | 3.91ms | High-precision simulations |
| 512 | 1.95ms | **Engine target** |

### TemporalFrameCount

- **CPU impact:** None — frame count only affects memory, not iteration cost.
- **Rollback cost:** When correction is applied, resimulate from rollback frame forward.
  Cost = (FramesSinceCorrection × SingleFrameCost).

---

## Runtime Configuration

### Initialization

Engine configuration is loaded from `*Defaults.ini` files in the project source directory (not the build directory).
Projects use the `GameManager` CRTP pattern — no manual `main()` required:

```cpp
class MyGame : public GameManager<MyGame> {
    const char* GetWindowTitle() const { return "My Game"; }
    bool PostInitialize(TrinyxEngine& engine) { /* spawn entities */ return true; }
};
TNX_IMPLEMENT_GAME(MyGame)
```

The `TNX_IMPLEMENT_GAME` macro generates `main()`, initializes the engine with the project's `TNX_PROJECT_DIR`
(set by CMake), loads all `*Defaults.ini` files from that directory, and calls `PostInitialize` on your game class.

**Example `GameDefaults.ini`:**

```ini
FixedUpdateHz = 512
TemporalFrameCount = 128
MAX_RENDERABLE_ENTITIES = 15000
MAX_JOLT_BODIES = 8000
MAX_CACHED_ENTITIES = 25000
InputPollHz = 1000
JobCacheSize = 16384
```

### What Can Change at Runtime

**Safe to adjust:** Nothing size-related — all partition and tier sizes are baked into slab allocations at startup.
The logic thread rate can be re-targeted by modifying `FixedUpdateHz` before calling `Run()`, but changing
it mid-simulation would break the temporal history interpretation.

---
