# Engine Configuration

> **Navigation:** [← Back to README](../README.md) | [← Data Structures](DATA_STRUCTURES.md) | [Current Status →](STATUS.md)

---

## EngineConfig Structure

All engine timing, threading, and memory parameters are configured through `EngineConfig`:

```cpp
struct EngineConfig
{
    // === Timing Configuration ===

    // Fixed Update Rate (Physics/Simulation)
    // Runs at deterministic timestep regardless of frame rate.
    // Common values: 60, 128, 256, 512
    int FixedUpdateHz = 128;

    // Input Polling Rate (Main Thread / Sentinel)
    // How often Sentinel samples SDL input events.
    int InputPollHz = 1000;

    // Network Tick Rate
    // How often to send/receive network packets.
    int NetworkUpdateHz = 30;

    // === Entity Budget Configuration ===
    //
    // Two fixed-size arenas, each with a dual-ended allocator:
    //
    //   Arena 1: Physics  [0 .. MaxPhysicsEntities)
    //     PHYS  (→) from 0             — physics-only bodies, triggers
    //     DUAL  (←) from MaxPhysics    — physics + render (players, props)
    //
    //   Arena 2: Cached  [MaxPhysicsEntities .. MaxCachedEntities)
    //     RENDER (→) from MaxPhysics   — render-only (particles, decals)
    //     LOGIC  (←) from MaxCached    — logic/rollback-only entities
    //
    // Constraints (validated at startup):
    //   MaxPhysicsEntities <= MaxCachedEntities

    // Total size of Arena 1 (Physics). PHYS + DUAL must fit within this.
    int MaxPhysicsEntities = 50000;

    // Total size of both arenas combined. Replaces MaxDynamicEntities.
    int MaxCachedEntities  = 100000;

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
    int TemporalFrameCount = 8;

    // Number of frames stored in the Volatile tier ring buffer.
    // Fixed at 5: gives Logic/4 headroom between thread tick rates.
    // (4 frames = Logic/2 — tighter, not recommended)
    // Not configurable — the math only works at 5.
    static constexpr int VolatileFrameCount = 5;

    // Number of GPU InstanceBuffers.
    // 5 decouples the VSync clock from the logic thread.
    // Chain broken: VSync → GPU buffer → CPU render blocks → slab lock → Logic stalls.
    static constexpr int GpuInstanceBufferCount = 5;

    // === Determinism Mode ===

    // When true: all physics-authoritative fields use Fixed32 (int32 fixed-point).
    //   - Simulation is bit-identical across all platforms and compilers.
    //   - Required for rollback netcode, GGPO, or any multiplayer with authoritative state.
    //   - Slight CPU overhead for multiply-shift vs native float (negligible with AVX2 int math).
    //
    // When false: fields use float32 directly (no Fixed32 wrapper).
    //   - Faster to author (no FixedMul, familiar float arithmetic)
    //   - Not cross-platform bit-identical, but fine for single-player, RPGs, offline games
    //   - Can still use space partitioning and camera-relative rendering (precision benefits remain)
    //
    // This is a compile-time selection baked into FieldProxy<T> — you cannot switch at runtime
    // without rebuilding component layouts.
    bool DeterministicSimulation = false;

    // === Helper Functions ===

    double GetFixedStepTime() const { return 1.0 / FixedUpdateHz; }
    double GetInputPollInterval() const { return 1.0 / InputPollHz; }
    double GetNetworkStepTime() const { return (NetworkUpdateHz > 0) ? 1.0 / NetworkUpdateHz : 0.0; }
    double GetTemporalHistorySeconds() const { return (double)TemporalFrameCount / (double)FixedUpdateHz; }

    int GetLogicPartitionSize() const
    {
        return MaxCachedEntities - MaxPhysicsEntities - MaxRenderEntities;
    }
};
```

---

## Partition Size Rules

The two-arena layout must satisfy:

```
MaxDualEntities + MaxPhysEntities <= MaxPhysicsEntities   // both physics buckets fit in Arena 1
MaxPhysicsEntities + MaxRenderEntities <= MaxCachedEntities  // arenas fit in total budget
```

Logic entities fill the remainder of Arena 2:
```
MaxLogicEntities = MaxCachedEntities - MaxPhysicsEntities - MaxRenderEntities
```

**Physics iterates Arena 1:** `PHYS[0..N_phys)` + `DUAL[MAX_PHYS-N_dual..MAX_PHYS)` + `STATIC`
**Render iterates:** `DUAL[MAX_PHYS-N_dual..MAX_PHYS)` + `RENDER[MAX_PHYS..MAX_PHYS+N_render)` + `STATIC`

Oversizing a partition or arena wastes memory but has no correctness impact. Undersizing causes a startup assertion
failure.

---

## Configuration Presets

### Single-Player / RPG (No Determinism Needed)

```cpp
EngineConfig rpgConfig;
rpgConfig.FixedUpdateHz           = 60;
rpgConfig.MaxPhysicsEntities      = 50000;
rpgConfig.MaxCachedEntities       = 100000;
rpgConfig.TemporalFrameCount      = 8;
rpgConfig.NetworkUpdateHz         = 0;
rpgConfig.DeterministicSimulation = false;  // float32 fields, no fixed-point overhead
```

**Use Case:** Single-player RPGs, narrative games, offline simulations. Full engine feature set
(space partitioning, GPU-driven rendering, tiered storage) without the determinism machinery.

---

### Lightweight (Rendering Focus)

```cpp
EngineConfig lightweightConfig;
lightweightConfig.FixedUpdateHz      = 60;
lightweightConfig.MaxPhysicsEntities = 25000;
lightweightConfig.MaxCachedEntities  = 50000;
lightweightConfig.TemporalFrameCount = 8;
lightweightConfig.NetworkUpdateHz    = 0;
```

**Use Case:** Single-player games, simple simulations, no networking.

---

### Balanced (Standard Game)

```cpp
EngineConfig balancedConfig;
balancedConfig.FixedUpdateHz      = 128;
balancedConfig.MaxPhysicsEntities = 50000;
balancedConfig.MaxCachedEntities  = 100000;
balancedConfig.TemporalFrameCount = 16;   // ~125ms history @ 128Hz
balancedConfig.NetworkUpdateHz    = 30;
```

**Use Case:** Most networked games, action games.

---

### Competitive (High-Frequency, Full Determinism)

```cpp
EngineConfig competitiveConfig;
competitiveConfig.FixedUpdateHz           = 512;
competitiveConfig.DeterministicSimulation = true;  // Fixed32 fields, rollback-safe
competitiveConfig.InputPollHz             = 1000;
competitiveConfig.MaxPhysicsEntities      = 30000;
competitiveConfig.MaxCachedEntities       = 50000;
competitiveConfig.TemporalFrameCount      = 128;  // ~250ms history @ 512Hz
competitiveConfig.NetworkUpdateHz         = 60;
```

**Use Case:** Fighting games, competitive shooters, esports titles.
Rollback depth = 250ms, enough for GGPO-style netcode at typical internet latency.

---

### Simulation (Maximum Entities)

```cpp
EngineConfig simulationConfig;
simulationConfig.FixedUpdateHz      = 60;
simulationConfig.MaxPhysicsEntities = 100000;
simulationConfig.MaxCachedEntities  = 250000;
simulationConfig.TemporalFrameCount = 8;
simulationConfig.NetworkUpdateHz    = 0;
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
VolatileSlab = MaxCachedEntities × HotBytesPerEntity × VolatileFrameCount (5)
             = 100,000 × 92B × 5 ≈ 46 MB
```

### Temporal Slab Size

```
TemporalSlab = MaxCachedEntities × HotBytesPerEntity × TemporalFrameCount
             = 100,000 × 92B × 8  ≈ 73.6 MB   (TemporalFrameCount = 8)
             = 100,000 × 92B × 128 ≈ 1.18 GB  (TemporalFrameCount = 128)
```

### Memory Table (MaxCachedEntities = 100k, 92 B/entity/frame)

| TemporalFrameCount | Temporal Slab | + Volatile (5) | Total Hot RAM |
|-------------------|--------------|----------------|---------------|
| 8 | 73.6 MB | 46 MB | ~120 MB |
| 16 | 147 MB | 46 MB | ~193 MB |
| 64 | 589 MB | 46 MB | ~635 MB |
| 128 | 1.18 GB | 46 MB | ~1.22 GB |

Add ~100 MB for cold components in archetype chunks + ECS metadata.

### GPU Memory

| Buffer | Size (100k entities) |
|--------|----------------------|
| InstanceBuffer × 5 | 5 × (MaxDynamic × FieldStride) ≈ 5 × 12 MB = 60 MB |
| Static Buffer | Static entity count × FieldStride ≈ few MB |
| Cumulative Dirty Bit Array | MaxDynamic / 8 × 2 (double-buffered) ≈ 25 KB |

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
MaxPhysicsEntities = 50000
MaxCachedEntities = 100000
InputPollHz = 1000
JobCacheSize = 1024
```

### What Can Change at Runtime

**Safe to adjust:** Nothing size-related — all partition and tier sizes are baked into slab allocations at startup.
The logic thread rate can be re-targeted by modifying FixedUpdateHz before calling Run(), but changing
it mid-simulation would break the temporal history interpretation.

---
