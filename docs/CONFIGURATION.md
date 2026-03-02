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
    // Partition layout within each tier's slab:
    //   DUAL   [0 .. MaxDualEntities)        — physics + render access
    //   PHYS   [0 .. MaxPhysEntities)        — physics-only
    //   RENDER [0 .. MaxRenderEntities)      — render-only
    //   LOGIC  [remainder of MaxDynamicEntities]
    //
    // Constraint (validated at startup):
    //   MaxDualEntities + MaxPhysEntities + MaxRenderEntities <= MaxDynamicEntities

    // Maximum total dynamic entities across all partitions
    int MaxDynamicEntities = 100000;

    // Entities in the Dual (physics + render) partition
    int MaxDualEntities    = 20000;

    // Entities in the Phys-only partition
    int MaxPhysEntities    = 10000;

    // Entities in the Render-only partition
    int MaxRenderEntities  = 30000;

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
        return MaxDynamicEntities - MaxDualEntities - MaxPhysEntities - MaxRenderEntities;
    }
};
```

---

## Partition Size Rules

The four partition sizes must satisfy:

```
MaxDualEntities + MaxPhysEntities + MaxRenderEntities <= MaxDynamicEntities
```

Logic entities get the remainder:
```
MaxLogicEntities = MaxDynamicEntities - MaxDualEntities - MaxPhysEntities - MaxRenderEntities
```

**Physics iterates:** `DUAL[0..N_dual)` → `PHYS[0..N_phys)` → `STATIC[0..N_static)` — 3 dense SIMD passes.
**Render iterates:** `DUAL[0..N_dual)` → `RENDER[0..N_render)` → `STATIC[0..N_static)` — 3 dense SIMD passes.

Oversizing a partition wastes memory but has no correctness impact. Undersizing causes a startup assertion failure.

---

## Configuration Presets

### Single-Player / RPG (No Determinism Needed)

```cpp
EngineConfig rpgConfig;
rpgConfig.FixedUpdateHz          = 60;
rpgConfig.MaxDynamicEntities     = 100000;
rpgConfig.MaxDualEntities        = 20000;
rpgConfig.MaxPhysEntities        = 10000;
rpgConfig.MaxRenderEntities      = 40000;
rpgConfig.TemporalFrameCount     = 8;
rpgConfig.NetworkUpdateHz        = 0;
rpgConfig.DeterministicSimulation = false;  // float32 fields, no fixed-point overhead
```

**Use Case:** Single-player RPGs, narrative games, offline simulations. Full engine feature set
(space partitioning, GPU-driven rendering, tiered storage) without the determinism machinery.

---

### Lightweight (Rendering Focus)

```cpp
EngineConfig lightweightConfig;
lightweightConfig.FixedUpdateHz       = 60;
lightweightConfig.MaxDynamicEntities  = 50000;
lightweightConfig.MaxDualEntities     = 10000;
lightweightConfig.MaxPhysEntities     = 5000;
lightweightConfig.MaxRenderEntities   = 20000;
lightweightConfig.TemporalFrameCount  = 8;
lightweightConfig.NetworkUpdateHz     = 0;
```

**Use Case:** Single-player games, simple simulations, no networking.

---

### Balanced (Standard Game)

```cpp
EngineConfig balancedConfig;
balancedConfig.FixedUpdateHz       = 128;
balancedConfig.MaxDynamicEntities  = 100000;
balancedConfig.MaxDualEntities     = 20000;
balancedConfig.MaxPhysEntities     = 10000;
balancedConfig.MaxRenderEntities   = 30000;
balancedConfig.TemporalFrameCount  = 16;   // ~125ms history @ 128Hz
balancedConfig.NetworkUpdateHz     = 30;
```

**Use Case:** Most networked games, action games.

---

### Competitive (High-Frequency, Full Determinism)

```cpp
EngineConfig competitiveConfig;
competitiveConfig.FixedUpdateHz          = 512;
competitiveConfig.DeterministicSimulation = true;  // Fixed32 fields, rollback-safe
competitiveConfig.InputPollHz         = 1000;
competitiveConfig.MaxDynamicEntities  = 50000;
competitiveConfig.MaxDualEntities     = 15000;
competitiveConfig.MaxPhysEntities     = 5000;
competitiveConfig.MaxRenderEntities   = 10000;
competitiveConfig.TemporalFrameCount  = 128;  // ~250ms history @ 512Hz
competitiveConfig.NetworkUpdateHz     = 60;
```

**Use Case:** Fighting games, competitive shooters, esports titles.
Rollback depth = 250ms, enough for GGPO-style netcode at typical internet latency.

---

### Simulation (Maximum Entities)

```cpp
EngineConfig simulationConfig;
simulationConfig.FixedUpdateHz       = 60;
simulationConfig.MaxDynamicEntities  = 250000;
simulationConfig.MaxDualEntities     = 50000;
simulationConfig.MaxPhysEntities     = 25000;
simulationConfig.MaxRenderEntities   = 100000;
simulationConfig.TemporalFrameCount  = 8;
simulationConfig.NetworkUpdateHz     = 0;
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
VolatileSlab = MaxDynamicEntities × HotBytesPerEntity × VolatileFrameCount (5)
             = 100,000 × 92B × 5 ≈ 46 MB
```

### Temporal Slab Size

```
TemporalSlab = MaxDynamicEntities × HotBytesPerEntity × TemporalFrameCount
             = 100,000 × 92B × 8  ≈ 73.6 MB   (TemporalFrameCount = 8)
             = 100,000 × 92B × 128 ≈ 1.18 GB  (TemporalFrameCount = 128)
```

### Memory Table (100k entities, 92 B/entity/frame)

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

```cpp
int main()
{
    EngineConfig config;
    config.FixedUpdateHz      = 512;
    config.TemporalFrameCount = 128;
    config.MaxDynamicEntities = 100000;
    config.MaxDualEntities    = 20000;
    config.MaxPhysEntities    = 10000;
    config.MaxRenderEntities  = 30000;
    config.InputPollHz        = 1000;

    TrinyxEngine& engine = TrinyxEngine::Get();
    if (engine.Initialize("My Game", 1920, 1080))
    {
        engine.Run();
    }
    return 0;
}
```

### What Can Change at Runtime

**Safe to adjust:** Nothing size-related — all partition and tier sizes are baked into slab allocations at startup.
The logic thread rate can be re-targeted by modifying FixedUpdateHz before calling Run(), but changing
it mid-simulation would break the temporal history interpretation.

---
