# Data Structures Reference

> **Navigation:** [← Back to README](../README.md) | [← Performance](PERFORMANCE_TARGETS.md) | [Configuration →](CONFIGURATION.md)

---

## FieldProxy — SoA with OOP Syntax

`FieldProxy<T, FieldWidth>` is the core component abstraction. Each proxy holds:
- `ReadArray` — frame T (current simulation state, read-only during update)
- `WriteArray` — frame T+1 (next state, written during update)
- `index` — current entity offset within the SoA arrays
- `mask` — AVX2 mask for `WideMask` partial-lane writes (zero-size base for `Scalar` mode)

`FieldWidth` has three modes:

| Mode | Entities/iteration | Use case |
|------|-------------------|----------|
| `Scalar` | 1 | Simple, safe, default |
| `Wide` | 8 (AVX2 lane) | Maximum throughput, count must be multiple of 8 |
| `WideMask` | 8 with tail mask | Non-multiple-of-8 tail chunks |

`FieldProxyMask<WIDTH>` is a zero-size base for `Scalar` mode (saves 32 bytes per field vs. always
storing `__m256i mask`). Wide and WideMask access the mask via `this->mask`.

```cpp
// Simplified structure (see FieldProxy.h for full implementation)
template <typename T, FieldWidth WIDTH = FieldWidth::Scalar>
struct FieldProxy : FieldProxyMask<WIDTH>
{
    T* ReadArray  = nullptr;
    T* WriteArray = nullptr;
    uint32_t index = 0;

    // OOP-style read (reads WriteArray for accumulate path)
    operator T() const { return WriteArray[index]; }

    // Compound assignment (reads and writes WriteArray)
    FieldProxy& operator+=(T value) { WriteArray[index] += value; return *this; }
    FieldProxy& operator-=(T value) { WriteArray[index] -= value; return *this; }
    FieldProxy& operator*=(T value) { WriteArray[index] *= value; return *this; }

    // Bind: copy ReadArray → WriteArray and cache index
    void Bind(T* read, T* write, uint32_t idx);

    // Advance: increment index, propagate copy (or SIMD step for Wide/WideMask)
    void Advance(uint32_t step);
};
```

**Key Properties:**
- All operators inline to direct array access — zero overhead
- `Bind()` copies frame T to frame T+1 at hydration; entity starts from its last known state
- `Advance()` increments the shared index (or 8 for Wide mode)
- Users write `transform.PositionX += velocity.VelocityX * dt` — looks like OOP, runs as SoA

---

## Component Definition Patterns

### Temporal (SoA) Component

Components with `TNX_TEMPORAL_FIELDS` decompose into SoA field arrays in the temporal/volatile slab.
The `SystemGroup` tag drives automatic entity partition placement.

```cpp
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct Transform
{
    FieldProxy<float, WIDTH> PositionX, PositionY, PositionZ;
    FieldProxy<float, WIDTH> RotationX, RotationY, RotationZ;
    FieldProxy<float, WIDTH> ScaleX,    ScaleY,    ScaleZ;

    // SystemGroup::None = partition-agnostic; entity's group comes from other components
    TNX_TEMPORAL_FIELDS(Transform, SystemGroup::None,
        PositionX, PositionY, PositionZ,
        RotationX, RotationY, RotationZ,
        ScaleX, ScaleY, ScaleZ)
};
TNX_REGISTER_COMPONENT(Transform)

template <FieldWidth WIDTH = FieldWidth::Scalar>
struct RigidBody
{
    FieldProxy<float, WIDTH> VelX, VelY, VelZ;
    FieldProxy<float, WIDTH> AngVelX, AngVelY, AngVelZ;

    // SystemGroup::Phys = entities with this component go into the Phys or Dual partition
    TNX_TEMPORAL_FIELDS(RigidBody, SystemGroup::Phys,
        VelX, VelY, VelZ, AngVelX, AngVelY, AngVelZ)
};
TNX_REGISTER_COMPONENT(RigidBody)

template <FieldWidth WIDTH = FieldWidth::Scalar>
struct Material
{
    FieldProxy<float, WIDTH> ColorR, ColorG, ColorB, ColorA;

    // SystemGroup::Render = entities with this component go into the Render or Dual partition
    TNX_TEMPORAL_FIELDS(Material, SystemGroup::Render,
        ColorR, ColorG, ColorB, ColorA)
};
TNX_REGISTER_COMPONENT(Material)
```

### Cold (Chunk) Component

Components without `TNX_TEMPORAL_FIELDS` live in archetype chunk memory (AoS layout).
No FieldProxy — plain POD members.

```cpp
struct HealthComponent
{
    float CurrentHealth;
    float MaxHealth;
    float Armor;

    TNX_REGISTER_FIELDS(HealthComponent, CurrentHealth, MaxHealth, Armor)
};
TNX_REGISTER_COMPONENT(HealthComponent)
```

### Universal Strip Component

Flags that need to be iterable across ALL entities in a single SIMD pass live in the universal strip,
outside the partition field zones. Declared with `TNX_UNIVERSAL_COMPONENT` (planned macro):

```cpp
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct TemporalFlags
{
    FieldProxy<int32_t, WIDTH> Flags;

    // TemporalFlagBits:
    //   Active = 1<<31  — entity exists and is in the simulation
    //   Dirty  = 1<<30  — modified this frame (drives GPU upload AND rollback resimulation blast radius)
    TNX_UNIVERSAL_COMPONENT(TemporalFlags, Flags)
};
```

---

## Entity Definition Pattern

Entities are CRTP structs templated on `FieldWidth`. The partition group is automatically derived from
the `SystemGroup` tags of the entity's components — **no manual group annotation on the entity itself**.

```cpp
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct CubeEntity : EntityView<CubeEntity, WIDTH>
{
    Transform<WIDTH> transform;
    RigidBody<WIDTH> body;
    Material<WIDTH>  material;

    // Partition group = Dual (has both Phys and Render components)

    FORCE_INLINE void PrePhysics(double dt)
    {
        transform.PositionX += body.VelX * static_cast<float>(dt);
        transform.PositionY += body.VelY * static_cast<float>(dt);
        transform.PositionZ += body.VelZ * static_cast<float>(dt);
    }

    TNX_REGISTER_SCHEMA(CubeEntity, EntityView, transform, body, material)
};
TNX_REGISTER_ENTITY(CubeEntity)
```

To opt into full rollback (Temporal tier vs Volatile tier), add `SimulationBody`:

```cpp
struct SimulationBody { /* empty marker, no fields */ };

// CubeEntity with SimulationBody → Temporal tier (N-frame rollback ring)
// CubeEntity without SimulationBody → Volatile tier (5-frame ring, no rollback)
```

### Inheritance Pattern

```cpp
template <typename Derived, FieldWidth WIDTH = FieldWidth::Scalar>
struct BaseCube : EntityView<Derived, WIDTH>
{
    Transform<WIDTH> transform;
    Material<WIDTH>  material;

    FORCE_INLINE void PrePhysics(double dt)
    {
        transform.RotationY += static_cast<float>(dt) * 0.7f;
    }

    TNX_REGISTER_SUPER_SCHEMA(BaseCube, EntityView, transform, material)
};

template <FieldWidth WIDTH = FieldWidth::Scalar>
struct SuperCube : BaseCube<SuperCube, WIDTH>
{
    RigidBody<WIDTH> body;

    FORCE_INLINE void PrePhysics(double dt)
    {
        BaseCube<SuperCube, WIDTH>::PrePhysics(dt);
        this->transform.PositionX += body.VelX * static_cast<float>(dt);
    }

    TNX_REGISTER_SCHEMA(SuperCube, BaseCube, body)
};
TNX_REGISTER_ENTITY(SuperCube)
```

---

## EntityView Pattern

CRTP base for all entity types. Provides hydration (binding FieldProxies to SoA arrays) and Advance:

```cpp
template <template <FieldWidth> class T, FieldWidth WIDTH = FieldWidth::Scalar>
class EntityView
{
public:
    uint32_t ViewIndex = 0;

    // Hydrate: bind all component FieldProxies to read/write arrays at index
    FORCE_INLINE void Hydrate(void** readArrays, void** writeArrays, uint32_t index);

    // Advance: increment ViewIndex (+ 8 for Wide, with mask for WideMask)
    FORCE_INLINE void Advance(uint32_t step = 1);
};
```

Iteration pattern (internal, generated by reflection system):

```cpp
template <typename T>
FORCE_INLINE void InvokePrePhysicsImpl(double dt, void** read, void** write, uint32_t count)
{
    T entityView;
    entityView.Hydrate(read, write, 0);

    uint32_t i = 0;
    // Wide pass (groups of 8)
    for (; i + 8 <= count; i += 8)
    {
        // WideMask setup not needed for full groups
        entityView.PrePhysics(dt);
        entityView.Advance(8);
    }
    // Scalar tail
    for (; i < count; ++i)
    {
        entityView.PrePhysics(dt);
        entityView.Advance(1);
    }
}
```

---

## SimFloat / FloatProxy — Determinism Alias

The determinism mode switch lives in one place. Entity authors never touch it:

```cpp
// SimFloat.h — the only file that changes between deterministic and float builds

#if TNX_DETERMINISTIC
    using SimFloat = Fixed32;
#else
    using SimFloat = float;
#endif

// FloatProxy resolves to the correct FieldProxy backing type automatically
template <FieldWidth WIDTH = FieldWidth::Scalar>
using FloatProxy = FieldProxy<SimFloat, WIDTH>;
```

Entity and component authors always write `FloatProxy<WIDTH>` — identical in both builds:

```cpp
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct Transform {
    FloatProxy<WIDTH> PosX, PosY, PosZ;  // Fixed32 or float, decided at compile time
    // ...
    TNX_TEMPORAL_FIELDS(Transform, SystemGroup::None, PosX, PosY, PosZ, ...)
};

// PrePhysics receives SimFloat dt — same syntax in both builds
FORCE_INLINE void PrePhysics(SimFloat dt) {
    transform.PosX += body.VelX * dt;   // unchanged regardless of backing type
}
```

**Where source changes are required when switching modes:**
Fixed32 needs an implicit constructor from float to cover literal assignment transparently:
```cpp
Fixed32(float f) : raw(static_cast<int32_t>(f * FIXED_UNITS_PER_METER)) {}
```
The compiler surfaces the remaining incompatibilities — trig functions, explicit float casts, logging
of raw values — as errors. These are the exact places "where the swap changes the outcome of what
float would have returned" and are found mechanically, not by audit.

---

## Fixed32 — Fixed-Point Value Type

Wraps `int32_t` with arithmetic operators that use `int64_t` intermediates to prevent overflow.
Entity authors write the same syntax as float — the multiply-shift is invisible.

```cpp
// Unit definition: 1 unit = 0.1mm
static constexpr int32_t FIXED_UNITS_PER_METER = 10000;
static constexpr int32_t FIXED_SCALE_SHIFT     = 14;    // 2^14 = 16384 ≈ scaling denominator

struct Fixed32
{
    int32_t raw = 0;

    // Construct from meters (at authoring time / spawn)
    static Fixed32 FromMeters(float m) { return { static_cast<int32_t>(m * FIXED_UNITS_PER_METER) }; }
    float ToMeters() const { return static_cast<float>(raw) / FIXED_UNITS_PER_METER; }

    Fixed32 operator+(Fixed32 b) const { return { raw + b.raw }; }
    Fixed32 operator-(Fixed32 b) const { return { raw - b.raw }; }
    Fixed32& operator+=(Fixed32 b)    { raw += b.raw; return *this; }
    Fixed32& operator-=(Fixed32 b)    { raw -= b.raw; return *this; }

    // Multiply: must use int64 intermediate
    Fixed32 operator*(Fixed32 b) const {
        return { static_cast<int32_t>((static_cast<int64_t>(raw) * b.raw) >> FIXED_SCALE_SHIFT) };
    }
};

// FieldProxy<Fixed32, WIDTH> — used for all position, velocity, force fields
```

**Platform determinism:** Integer arithmetic produces identical bits on x86, ARM, any compiler,
any optimization level. This is required for rollback netcode and makes Fixed32 the mandatory type
for all physics-authoritative simulation data.

**When not needed:** Single-player and non-networked games can opt out per `EngineConfig` and receive
a `float`-backed `FieldProxy<float, WIDTH>` instead (see Configuration — Determinism Mode).

---

## ConstraintEntity

Constraints are independent entities in the LOGIC partition. They define relationships between two
bodies and are the foundation for both transform attachment and physics joints.

```cpp
struct ConstraintEntity
{
    uint32_t       BodyA;       // entity partition index (UINT32_MAX = static world anchor)
    uint32_t       BodyB;       // entity partition index (UINT32_MAX = static world anchor)
    ConstraintType Type;        // see enum below
    uint16_t       BoneA;       // bone on BodyA (UINT16_MAX = entity root pivot)
    uint16_t       BoneB;       // bone on BodyB (UINT16_MAX = entity root pivot)
    Fixed32        AnchorA[3];  // local-space anchor point on BodyA
    Fixed32        AnchorB[3];  // local-space anchor point on BodyB
    Fixed32        LimitMin[3]; // lower DOF limits (translation mm or angle units)
    Fixed32        LimitMax[3]; // upper DOF limits
    Fixed32        Stiffness;   // spring/soft constraints
    Fixed32        Damping;     // spring/soft constraints
};

enum class ConstraintType : uint8_t
{
    Rigid,          // all 6 DOF locked — rigid transform inheritance (weapon to hand, etc.)
    RigidWithScale, // rigid + inherit scale
    PositionOnly,   // translation locked, rotation free
    Hinge,          // 1 rotational DOF free (door, axle)
    BallSocket,     // 3 rotational DOF free, translation locked (shoulder, hip)
    Prismatic,      // 1 translational DOF free, rotation locked (piston, slider)
    Distance,       // fixed distance, all rotation free (chain link)
    Spring,         // distance constraint with stiffness/damping
};
```

**Rigid attachment is a degenerate constraint** — `Type=Rigid` with zero limits. The render thread
resolves these before GPU upload (world transform inheritance). All other types are resolved by the
physics solver.

**Entities with a `Type=Rigid` ConstraintEntity as BodyA do not get independent physics bodies.**
For physics-simulated attachment (ragdoll, rope, soft body), use non-Rigid types — the solver
resolves them via impulses.

---

## TemporalFlags

Live in the universal strip (one contiguous `int32` array per frame, covering ALL dynamic entities):

```cpp
enum class TemporalFlagBits : int32_t
{
    Active = 1 << 31,   // Entity exists and is in the simulation
    Dirty  = 1 << 30,   // Modified this frame; drives GPU upload + rollback resimulation
};
```

Logic sets `Dirty` (bit 30) via `fetch_or(relaxed)` after updating each chunk.
Render reads `Dirty` bits to build the GPU upload set, then clears them after upload.

During rollback, the dirty set from a correction expands naturally through update logic —
the exact set of entities affected by the correction is computed automatically.

---

## GPU InstanceBuffer Layout (SoA)

The GPU InstanceBuffer is Structure-of-Arrays, indexed by semantic:

```
InstancesAddr[ (semantic - 1) * OutFieldStride + outIdx ]
```

Where `outIdx` is the compacted index from the scatter pass, and `semantic` maps to:
- 1: PositionX, 2: PositionY, 3: PositionZ
- 4: RotationX, 5: RotationY, 6: RotationZ
- 7: ScaleX, 8: ScaleY, 9: ScaleZ
- 10: ColorR, 11: ColorG, 12: ColorB, 13: ColorA

The vertex shader reads all 13 fields per instance, reconstructs the TRS matrix inline.

**Five GPU InstanceBuffers** are maintained in flight. Render thread always finds a free buffer
without blocking on presentation — decouples the VSync clock from the logic thread's slab locks.

---

## GpuFrameData.slang

Shared header included by all Slang shaders (`#include "GpuFrameData.slang"`). Contains:
- `ViewProj` — `float4x4`, column-major from C++ → row-major in Slang via `M[r][c] = ViewProj[c*4+r]`
- `VerticesAddr` — Buffer Device Address for cube vertex data
- `InstancesAddr` — Buffer Device Address for the InstanceBuffer SoA
- `ScanAddr` / `PrefixAddr` — BDAs for the predicate and prefix sum buffers
- `OutFieldStride` — stride between field planes in the InstanceBuffer SoA
- `EntityCount`, `DrawArgs` — entity count and indirect draw arguments buffer

---

## DrawCall / State Sorting (Planned)

**64-Bit Sort Key:**

```cpp
union SortKey
{
    uint64_t value;
    struct
    {
        uint64_t mesh     : 16;  // 65536 unique meshes
        uint64_t material : 16;  // 65536 unique materials
        uint64_t pipeline : 12;  // 4096 pipelines (shader/blend state)
        uint64_t depth    : 16;  // Distance from camera (quantized)
        uint64_t layer    : 4;   // 0=Background, 1=Opaque, 2=Transparent, 3=UI
    };
};
```

Radix sort on the GPU after the scatter pass groups draw calls by Layer → Depth → Pipeline → Material → Mesh,
minimizing GPU state changes.

---
