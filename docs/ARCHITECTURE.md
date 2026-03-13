# TrinyxEngine Architecture

> **Navigation:** [← Back to README](../README.md) | [Performance Targets →](PERFORMANCE_TARGETS.md)

---

# Threading Model: The Trinyx Trinity

## Overview

TrinyxEngine uses a **three-thread architecture** with job-based parallelism:

1. **Sentinel (Main Thread):** 1000Hz input polling, window + Vulkan object lifetime, frame pacing
2. **Brain (Logic Thread):** 512Hz fixed timestep coordinator + job distribution
3. **Encoder (Render Thread):** Variable-rate render coordinator + job distribution

**Key Design:** Brain and Encoder are **coordinators**, not workers. They initialize frames, distribute work to
the job system, and act as workers themselves while jobs are pending.

## Job System Architecture

### Core Count Distribution (8-core example)

- **Sentinel Thread:** 1 dedicated core (input + Vulkan lifetime)
- **Encoder Thread:** 1 dedicated core (render coordination + render worker)
- **Brain Thread:** 1 dedicated core (logic coordination + logic worker)
- **Workers:** 5 cores (process tasks from both queues via work-stealing)

### Queue Architecture

Four priority queues with affinity rules:

- **Logic Queue** — PrePhysics/PostPhysics per-chunk jobs (Brain produces, all consume)
- **Render Queue** — GPU upload/compute dispatch (Encoder produces, all consume)
- **Physics Queue** — Jolt physics solver jobs (Workers produce, workers consume)
- **General Queue** — Everything else + overflow from full queues

**Affinity:**

- **Brain Thread:** Produces Physics jobs, then steals from Physics queue while waiting
- **Encoder Thread:** Produces Render jobs, then steals from Render queue while waiting
- **Workers:** Pull from all queues (work-stealing). 25% dedicated to Jolt queue by default.

### Job Types

**Logic Jobs:** PrePhysics updates (per-chunk), PostPhysics updates

**Physics Jobs:** Jolt solver steps, collision detection, broad-phase (bridged via JoltJobSystemAdapter)

**Render Jobs:** GPU upload (dirty entity delta), compute dispatch (predicate/prefix_sum/scatter),
frustum culling (planned), sort key generation (planned), command encoding

---

# Memory Model: Tiered Storage

## Overview: Four Storage Tiers

Entity component data lives in one of four tiers based on access pattern and rollback requirements:

| Tier     | Structure                | Frames    | Rollback | Use Case                                     |
|----------|--------------------------|-----------|----------|----------------------------------------------|
| Cold     | Archetype chunks (AoS)   | 0         | No       | Rarely-updated, non-iterable data            |
| Static   | Separate read-only array | 0         | No       | Geometry/mesh data, never changes            |
| Volatile | SoA ring buffer          | 3         | No       | Cosmetic entities, ambient AI, particles     |
| Temporal | SoA ring buffer          | max(8, X) | Yes      | Networked, simulation-authoritative entities |

**Cold** components (no `TNX_TEMPORAL_FIELDS`) live only in Archetype chunks (still SoA decomposed for hydration
iteration).
Available in Pre/Post Physics and ScalarUpdate iteration. No rollback, changes are permanent.

**Static** entities get a dedicated read-only SoA array. No rollback overhead. Physics and render include
static data as a third dense pass after their partition ranges. Not yet implemented — pending asset import pipeline.

**Volatile** entities use a 3-frame triple-buffer per field. One frame is being written by Logic, one is
being read by the Encoder, and one is free. The renderer always has access to the latest complete frame
without blocking the Logic thread. No rollback; these entities are not networked.

**Temporal** entities use an N-frame ring buffer where N = `max(8, rollback_depth_at_FixedUpdateHz)`.
Full history for rollback netcode, lag compensation, and replay. Rollback interaction with Jolt Physics
(which ticks at 1/PhysicsUpdateInterval of the Logic rate) is an open design problem.

**Note:** If `TNX_ENABLE_ROLLBACK` is not defined, Temporal entities fall back to 3-frame triple-buffer
(same as Volatile), saving significant memory for games that do not require rollback.

---

## Volatile vs Temporal Classification

The tier is determined by the component's registration macro, not by a marker on the entity:

- `TNX_TEMPORAL_FIELDS(...)` → component fields go in the **Temporal** tier (N-frame rollback ring)
- `TNX_VOLATILE_FIELDS(...)` → component fields go in the **Volatile** tier (Triple buffer, no rollback)

An entity's effective tier is the highest tier of any of its components. An entity carrying a
`RigidBody` (Temporal) is a Temporal entity; an entity with only `ColorData` (Volatile) is Volatile.
Cold components (`TNX_REGISTER_FIELDS` — no tier) contribute no slab storage and do not affect the
entity's tier classification.

---

## Partition Design: Dual-Ended Arenas

The slab is divided into two fixed-size arenas by `EngineConfig`. Within each arena, two buckets grow
inward from opposite ends. Arena boundaries (`MaxPhysicsEntities`, `MaxCachedEntities`) and maximum
bucket sizes are all predetermined at startup.

```
Arena 1: Physics  [0 .. MAX_PHYSICS_ENTITIES)
  PHYS  (→) starts at 0            — physics-only (triggers, invisible movers)
  DUAL  (←) starts at MAX_PHYSICS  — physics + render (players, AI, physics props)

Arena 2: Cached  [MAX_PHYSICS_ENTITIES .. MAX_CACHED_ENTITIES)
  RENDER (→) starts at MAX_PHYSICS — render-only (particles, decals, ambient props)
  LOGIC  (←) starts at MAX_CACHED  — logic/rollback-only entities
```

Config constraints (validated at startup):
```
assert(MaxDualEntities + MaxPhysEntities <= MaxPhysicsEntities)
assert(MaxPhysicsEntities + MaxRenderEntities <= MaxCachedEntities)
```

**System iteration patterns — two dense passes each, no pointer chasing:**

- Physics iterates Arena 1: `PHYS[0..N_phys)` + `DUAL[MAX_PHYSICS-N_dual..MAX_PHYSICS)` + `STATIC`
  100% physics entities; render-only and logic-only data are outside this range entirely.
- Render iterates: `DUAL[MAX_PHYSICS-N_dual..MAX_PHYSICS)` + `RENDER[MAX_PHYSICS..MAX_PHYSICS+N_render)` + `STATIC`
  The DUAL tail and RENDER head are contiguous at the arena boundary — effectively one scan.

The physics solver iterates Arena 1 exclusively. It sees a dense wall of memory containing 100%
of its relevant entities, with no render-only or logic-only data anywhere in its access range. Because 32-bit slots
correspond to an entity global ID this can lead to holes and gaps, but the arena layout minimizes the holes and makes
the significant gap the one between Phys and Dual, allowing processes to fly over it. Might be worth swapping Physics
and Render alloocation slots in the future depending on if Renderer or physics benefits more for the gapless iteration.

---

## Entity Group Auto-Derivation

The partition group (Dual/Phys/Render/Logic) is computed automatically at `TNX_REGISTER_ENTITY`
time (static initialization), derived from the SystemGroup tags on each component:

```cpp
// Component system group annotations — no manual entity group annotation needed
TNX_TEMPORAL_FIELDS(RigidBody, SystemGroup::Phys,   VelX, VelY, VelZ)
TNX_TEMPORAL_FIELDS(Material,  SystemGroup::Render, ColorR, ColorG, ColorB)
TNX_TEMPORAL_FIELDS(Transform, SystemGroup::None,   PosX, PosY, PosZ)  // partition-agnostic
TNX_TEMPORAL_FIELDS(Stats,     SystemGroup::Logic,  Health, Ammo)
```

Derivation rule (applied once at static-init when `TNX_REGISTER_ENTITY(T)` fires):

| Components present            | → Entity Group                                                                                                                |
|-------------------------------|-------------------------------------------------------------------------------------------------------------------------------|
| Phys AND Render               | Dual                                                                                                                          |
| Phys only                     | Phys                                                                                                                          |
| Render only                   | Render                                                                                                                        |
| Temporal but neither          | Logic                                                                                                                         |
| No temporal components at all | Currently Flags are still temporal, would be nice to get them into the chunk if there's no other temporal components possibly |

`TNX_REGISTER_SCHEMA(T)` takes **no manual group annotation** — fully automatic. Giving users a
manual override is giving them a foot gun to shoot gaps into their partition regions with.

**Valuable Note:** The engine will provide a wide array of component and entity options out of the box, meaning that
developers can create gameplay without having to know the intricacies of setting up their own components or entities if
they don't want to.

---

## Bitplanes:

There are several bitplanes dedicated to tracking which components an entity has, if the entity is dirty, etc.

**Macro-gap skipping:** The universal strip is scanned 64 entities at a time (one 64-bit word). If the
entire word is zero (all 64 entities inactive), the branch predictor instantly skips that block — no
field data is touched. This covers the unused space between the PHYS and DUAL buckets within Arena 1
and sparse regions at startup with no measurable overhead.

**Micro-gap execution:** When a word has mixed active/inactive entities, `FieldProxy` uses AVX2 masked
loads/stores (8-wide, branchless) to keep the pipeline saturated even in partially sparse regions.

The dirty bit doubles as the GPU upload trigger and (during rollback) as a selective resimulation marker —
the dirty set from any correction expands naturally through update logic to the exact blast radius.

---

## Entity Flags:

Current implementation uses `TemporalFlagBits::Active = 1<<31`. This is used by the GPU compute shader to determine
which entities to draw. plus 31 other bits for future entity specific flags.

---

# Fixed-Point Coordinate System

## Motivation

Float32 within a 1km space partition cell has ≈8mm precision and is non-deterministic across platforms
(different FPU rounding modes, compiler optimizations, FMA behavior). For rollback netcode — where two
clients running the same inputs must produce bit-identical simulation state — this is unacceptable.

Fixed-point int32 is perfectly deterministic on every platform, every compiler, every optimization level.
Integer arithmetic produces identical bits on x86, ARM, any future target. This is the gold standard for
deterministic simulation and the reason fighting game engines (which require frame-perfect rollback) have
used it for decades. _This will need more testing as it sidesteps the benefits of how many floating-point ALUs modern
CPUs have._

## Unit Definition

```
1 unit = 0.1mm (100 micrometers)
1 meter   =     10,000 units
1 km      = 10,000,000 units
int32 max = 2,147,483,647 units = ±214 km

Cell size (1km): max value = 5,000,000 units  →  430× headroom before overflow
```

Unit definition is configurable in the EngineDefaults.ini.

## Fixed32 Value Type

A `Fixed32` wrapper around `int32_t` with overloaded arithmetic handles the multiply-shift pattern
transparently. Entity authors write natural syntax; the engine handles int64 intermediates:

```cpp
// Multiply: (a × b) needs 64-bit intermediate to avoid overflow
inline Fixed32 operator*(Fixed32 a, Fixed32 b) {
    return Fixed32(static_cast<int32_t>(
        (static_cast<int64_t>(a.raw) * b.raw) >> FIXED_SCALE_SHIFT
    ));
}

// Entity authors write exactly the same syntax as float:
transform.PosX += body.VelX * dt;   // Fixed32 * Fixed32 → Fixed32, no overflow
```

`FieldProxy<Fixed32, WIDTH>` — all position, velocity, and force fields use this type. because we have the FieldProxy we
can have a build variable to replace FieldProxy<float> with FieldProxy<fixed32> at runtime, no code changes required.

## Quaternion Representation

Quaternion components live in [-1, 1]. Store as int32 in 1.30 fixed-point format
(1 sign bit, 30 fraction bits, 1 bit for accidental overhead during math). Quaternion multiplication is pure integer
math with int64
intermediates — no trig in the inner loop. Trig only appears when creating a quaternion
from Euler angles (rare: at spawn or explicit rotation assignment).

## Coordinate Layers

```
Cell-relative fixed-point (int32):
  Entity positions, velocities, forces — all logic and physics

Cell world origin (float64 or int64):
  Absolute world position of each cell's origin
  Stored in Registry/World, never in entity SoA
  
  stored as a single int64, 20 bits for value + 1 bit for sign
  gives us a total world size of ~45M Km at 0.1mm precision.

Camera world position (float64 or int64):
  Maintained by logic thread, published each frame
```

## Render Thread Conversion (Upload Time)

The only lossy step — done once per dirty entity per frame by the render thread:

```cpp
// Camera-relative float32 for GPU (precision loss is acceptable: ≈0.05mm at 1km)
float32 gpu_x = float32(
    double(cell_world_origin.x - camera_world_pos.x)   // float64 subtraction (CPU, free)
    + double(entity.PosX.raw) * 0.0001                 // fixed-point units → meters
);
```

GPU receives small float32 (camera-relative, ≤±1km). Full float32 precision applies. Full throughput.
No doubles on the GPU.

## Jolt Physics Bridge (Temporary)

Jolt uses float32 internally. At the physics boundary:

```
Our fixed-point int32 → float32 cell-relative → Jolt body position
Jolt result float32   → int32 fixed-point     → our slab
```

At cell-relative scale (≤±500m from cell center), float32 precision is ≈0.05mm — finer than our
0.1mm unit definition. The bridge is lossless. When Jolt is replaced with a custom solver, the
bridge goes away and the fixed-point data feeds the solver directly. Needs better testing, but puts
a major wrinkle in our determinism.

---

# Constraint System

## Design: Constraint Pool (Separate AoS, Not in Temporal Slab)

Constraints are structural metadata — which body, which anchor, which type. They don't evolve
frame-to-frame and don't need rollback history (probably). The *resolved result* of constraints (world transforms
of constrained entities) lives in the temporal SoA as normal entity data. The constraint relationships
themselves live in a separate flat AoS pool.

AoS is correct here (vs SoA for entity fields) because the solver always reads all fields of one
constraint together — it never needs "all BodyA values" in isolation. Cache access pattern matches AoS.

Developers who need temporal/dynamic constraints (constraints that change per-frame, need history,
or need rollback) can author their own constraint components using the standard temporal SoA system.
The engine doesn't pay for that complexity by default.

```
ConstraintPool (flat AoS, owned by Registry, separate from temporal slab):

  ConstraintData[MAX_CONSTRAINTS]:
    uint32_t       BodyA        — entity partition index (UINT32_MAX = static world anchor)
    uint32_t       BodyB        — entity partition index (UINT32_MAX = static world anchor)
    ConstraintType Type         — Rigid, Hinge, BallSocket, Prismatic, Distance, Spring, ...
    uint16_t       BoneA        — bone on BodyA (UINT16_MAX = entity root pivot)
    uint16_t       BoneB        — bone on BodyB (UINT16_MAX = entity root pivot)
    Fixed32        AnchorA[3]   — local-space anchor on BodyA
    Fixed32        AnchorB[3]   — local-space anchor on BodyB
    Fixed32        LimitMin[3]  — lower DOF limits (translation or angle)
    Fixed32        LimitMax[3]  — upper DOF limits
    Fixed32        Stiffness    — spring/soft constraints
    Fixed32        Damping      — spring/soft constraints

  uint32_t Count
  uint32_t ByBodyA[MAX_CACHED_ENTITIES]   — O(1) lookup: "what constraint has entity X as BodyA?"
                                            UINT32_MAX = entity is unconstrained (physics root)
```

Why a separate AoS pool rather than a component on the child:
- Rigid attachment (weapon to hand) is just `Type=Rigid, all 6 DOF locked` — a degenerate case of the
  general form, not a special system
- World-anchored constraints (spring to fixed point in space) are natural: `BodyA = world`
- Multiple constraints between two bodies are possible (door: hinge + spring, no component slot conflict)
- Every Jolt joint type maps directly to a `ConstraintType` enum value
- Replacing the physics solver means replacing the solver loop, not the data layout

## Constraint Types

```cpp
enum class ConstraintType : uint8_t {
    Rigid,          // all 6 DOF locked — rigid attachment, transform inheritance
    RigidWithScale, // rigid + scale inherited (careful: breaks physics proxy)
    PositionOnly,   // 3 translation DOF locked, rotation free
    Hinge,          // 1 rotational DOF free (door, wheel on axle)
    BallSocket,     // 3 rotational DOF free, translation locked (shoulder joint)
    Prismatic,      // 1 translational DOF free, rotation locked (piston, slider)
    Distance,       // fixed distance between anchors, all rotation free (rope segment)
    Spring,         // distance constraint with stiffness/damping (soft attach, cloth edge)
};
```

## Naming: ConstraintEntity vs Jolt Constraints

Jolt's internal "constraints" (its joint system) are a different concept. To avoid confusion:
- **ConstraintEntity** (our system) = relationship between two ECS entities, drives transform and physics
- **Jolt joints** = Jolt's internal constraint solver, used only while Jolt is the physics backend

When Jolt is replaced, Jolt joints disappear. ConstraintEntities remain — the custom solver reads them.

## Render Thread Rigid Attachment Pass

Before uploading dirty entity deltas, the render thread resolves `Type=Rigid` constraints:

```
For each ConstraintEntity where Type == Rigid:
    WorldTransform[BodyA] = WorldTransform[BodyB]  (resolved via BoneB if BoneB != INVALID)
                            ⊗ LocalOffset(AnchorB → AnchorA)
    Mark BodyA dirty if BodyB was dirty this frame
```

Ordering: BodyB (parent) must be processed before BodyA (child). Two-pass iteration covers depth ≤ 2
(the vast majority of real attachment hierarchies). Cap at 4 passes, assert if exceeded.

## Physics: Only Roots Are Simulated

Entities with a `Type=Rigid` ConstraintEntity pointing to them as BodyA are **not** given independent
physics bodies. They inherit their transform from BodyB. The physics solver only creates bodies for
root entities (no ConstraintEntity where they are BodyA with Type=Rigid).

Entities that need independent physics AND follow a parent (ragdoll limbs, physically-simulated rope
segments) use non-Rigid constraint types — the solver resolves them via impulses/forces, not transform
copy. This is the distinction between transform attachment (render thread, free) and physics constraints
(solver, paid in simulation cost).

---

# Jolt Physics Sync Architecture

## Overview

~~Jolt runs on the Brain thread during the physics step of the fixed-timestep loop~~. That was naive
and does not work well, especially at 512Hz. Jolt now runs at a customizable fraction of the fixed update rate.
The physics step is initialted from a worker job, and has it's dedicated queue and workers.
By default the engine uses 512Hz Logic with 8 fixed steps per Physics step. This allows the physics sim
to run at 64Hz and asynchronously of other gameplay logic. Because this is configurable a game with
a need for high physics accuracy and low entities can bump this value 1:1 if desired. Because the physics step
is tied directly to fixed update rate it is still theoretically deterministic.

Physics entities are pushed into Jolt sim on (currentFrame % fixedStepPerPhysics == 0) as well as initiating
the physics step. Pulling transforms from jolt is done on (currentFrame % fixedStepPerPhysics == fixedStepPerPhysics -
1)
this will lock on the physics job counter before pulling if physics hasn't finished updating, allowing the
Logic thread to help with physics jobs.

**Jolt owns the physics state** (position, rotation, velocity) for all simulated bodies. The ECS does not push
state to Jolt every frame — Jolt's integrator advances bodies internally. The ECS only writes
to Jolt on explicit overrides (spawn, teleport, applied impulses). After each step, we pull
results from **awake bodies only** back into SoA WriteArrays.

```
PrePhysics → [Override if needed] → Jolt Step → Pull (awake only) → Collision Events → PostPhysics
```

## Overrides (ECS → Jolt, Event-Driven)

The ECS writes to Jolt bodies only when gameplay logic explicitly demands it — not per-frame:

- **Spawn**: `CreateAndAddBody()` with initial transform/velocity from entity data
- **Teleport**: `SetPositionAndRotation()` when gameplay moves an entity non-physically
- **Applied forces/impulses**: `AddForce()`, `AddImpulse()` from PrePhysics logic
- **Kinematic targets**: `MoveKinematic()` for animated platforms, elevators, etc.

```cpp
// Example: teleport (rare, event-driven)
JPH::BodyInterface& bi = physicsSystem.GetBodyInterfaceNoLock();
bi.SetPositionAndRotation(bodyID, pos, rot, JPH::EActivation::Activate);

// Example: gameplay impulse in PrePhysics
bi.AddImpulse(bodyID, JPH::Vec3(ix, iy, iz));
```

**Key API details:**

- `GetBodyInterfaceNoLock()` — Brain thread is the only writer; no lock needed
- `SetPositionAndRotation` — single call, avoids double broad-phase update vs separate Set calls
- Jolt uses `JPH::Quat(x, y, z, w)` constructor order — matches our `RotQx/Qy/Qz/Qw` field order

## Jolt Step

```cpp
physicsSystem.Update(fixedDt, collisionSteps, tempAllocator, jobSystem);
```

- `fixedDt` = 1.0/512.0 (matches Brain tick rate)
- `collisionSteps` = calculated from fixed Update rate and fixedStepsPerPhysics to target the suggested 60FPS step rate
- `tempAllocator` = `JPH::TempAllocatorImpl` with pre-allocated scratch buffer (16MB default).
  Jolt's temp allocator is a linear bump allocator that resets each step — no heap allocation
  in the inner loop. Size it for worst-case broad-phase during development, shrink later.
- `jobSystem` = adapter that submits to our LogicQueue (Jolt jobs run on Brain + Workers)

## Pull (Jolt → ECS, Awake Bodies Only)

After the step, read results back into SoA WriteArrays for **awake bodies only**. Sleeping bodies
haven't moved — their SoA data is already correct from the last frame they were awake.

```cpp
JPH::BodyIDVector activeIDs;
physicsSystem.GetActiveBodies(JPH::EBodyType::RigidBody, activeIDs);

JPH::BodyInterface& bi = physicsSystem.GetBodyInterfaceNoLock();

for (JPH::BodyID bodyID : activeIDs) {
    uint32_t entityIdx = bodyToEntity[bodyID.GetIndex()];

    // Position + rotation in one call (avoids double body lock)
    JPH::RVec3 pos;
    JPH::Quat rot;
    bi.GetPositionAndRotation(bodyID, pos, rot);

    WriteArray_PosX[entityIdx] = float_to_fixed(pos.GetX());
    WriteArray_PosY[entityIdx] = float_to_fixed(pos.GetY());
    WriteArray_PosZ[entityIdx] = float_to_fixed(pos.GetZ());

    WriteArray_RotQx[entityIdx] = rot.GetX();
    WriteArray_RotQy[entityIdx] = rot.GetY();
    WriteArray_RotQz[entityIdx] = rot.GetZ();
    WriteArray_RotQw[entityIdx] = rot.GetW();

    // Mark dirty for GPU upload
    WriteArray_Flags[entityIdx] |= TemporalFlagBits::Dirty;
}
```

**Only transforms are pulled.** Velocities stay in Jolt — they're Jolt's internal state, not SoA
fields. If gameplay logic needs velocity (e.g. speed-dependent VFX), it queries Jolt directly
during its Scalar update via `BodyInterface::GetLinearVelocity(bodyID)`.

**Awake-only pull** means a scene with 50K physics bodies where only 200 are moving pays for 200
pulls, not 50K. Sleeping bodies are the common case (stacked crates, settled debris, resting props).

**Scattered reads:** Using GetActiveBodies() to get a list of active bodies. Bodies are stored in AoS,
so we pull groups of 4 bodies translation and rotation, SIMD transpose and write to a field scratch
buffer. after we've built our SoA arrays of modified transforms we use the job system to push the values
back to their respective field array positions.

## Collision Events

Jolt fires collision callbacks during `Update()`. We buffer these in a thread-local ring:

```cpp
class ContactListener : public JPH::ContactListener {
    // Called during Jolt step (potentially from worker threads)
    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& settings) override
    {
        // Buffer to thread-local ring — no locks
        threadLocalContactBuffer.Push({b1.GetID(), b2.GetID(), manifold.mWorldSpaceNormal, ...});
    }
};
```

After Pull, Brain drains the contact buffers and dispatches `OnCollision` events to entity update
functions. Contacts are mapped back to entity IDs via a `BodyID → EntityID` lookup table maintained
during Push (body creation/destruction).

## Body Lifecycle

Bodies are created/destroyed in response to entity lifecycle events, not per-frame:

- **Entity spawn** (with physics component): create Jolt body, store BodyID↔EntityID mapping
- **Entity destroy**: remove Jolt body via `BodyInterface::DestroyBody()`
- **Entity deactivation** (Active flag cleared): `BodyInterface::DeactivateBody()` — Jolt skips it in broadphase
- **Entity reactivation**: `BodyInterface::ActivateBody()`

Body creation uses `BodyInterface::CreateAndAddBody()` — single call, avoids the
create-then-add pattern that requires an extra lock acquisition.

## Threading Considerations

- Push and Pull run on the Brain thread only (single writer to SoA WriteArrays)
- Jolt's internal `Update()` run via a job and spawns jobs via JoltJobSystemAdapter onto the Jolt queue
- Contact callbacks fire from Jolt worker threads → must use thread-local buffers (no shared state)
- No GPU thread touches Jolt state — render thread reads SoA fields only

---

## Current Implementation Status

The tiered partition design (Cold/Static/Volatile/Temporal with dual-ended arena layout) is **implemented**.
`TemporalComponentCache` provides N-frame SoA ring buffers per field with Active and Dirty bit tracking.
The delta-upload path is tracked but not yet wired to the GPU upload (currently full-slab copy).

Jolt Physics v5.5.0 is integrated: `JoltJobSystemAdapter` bridges onto the Jolt job queue, `JoltBody`
cold component provides shape/motion/mass data, and `FlushPendingBodies`/`PullActiveTransforms` sync
ECS ↔ Jolt each physics step.

---

# Speculative Presentation & Rollback Reconciliation

## Speculative Presentation

TrinyxEngine plays effects immediately based on the predicted game state to preserve crisp 0-ping feel.
Sound effects, particle spawns, and visual feedback fire at prediction time — not at server confirmation
time. Mispredictions are handled gracefully during rollback rather than avoided by delaying presentation.

## Event Buffer

A ring buffer of `TemporalEvents` (audio triggers, particle spawns, VFX handles) maps simulation events
to their presentation handles, timestamped by logic frame:

```cpp
struct TemporalEvent {
    uint32_t LogicFrame;  // frame the event was triggered
    uint32_t EntityID;    // entity that triggered it
    uint32_t EventType;   // audio, VFX, particle, etc.
    uint32_t Handle;      // presentation system handle for fade/cancel
};
```

## Presentation Reconciler (Planned)

When rollback occurs, the `PresentationReconciler` diffs events from the abandoned timeline against
the corrected timeline:

1. **Orphaned events** — fired in the abandoned timeline but absent from the corrected one (e.g., a
   predicted gunshot that never happened): issue an **Anti-Event**.
2. **New events** — present in the corrected timeline but absent from the prediction: play them now.
3. **Matching events** — already played correctly; no action needed.

## Anti-Events: Graceful Decay

Instead of instantly culling orphaned effects (which causes audio pops or visual hitches), Anti-Events
instruct the respective system to fade out rapidly but smoothly:

```
AudioSystem::RapidFadeOut(handle)   → ~20ms linear fade
ParticleSystem::SoftCancel(handle)  → freeze emission, drain existing particles
VFXSystem::RapidDecay(handle)       → accelerate decay curve to zero
```

The frame after rollback is visually continuous. A brief imperceptible fade replaces the instantaneous
pop, which is unnoticeable at normal play speeds.

---

# GPU Upload Architecture

## Cumulative Dirty Bit Array

A dense bit array external to the frame ring, double-buffered:

```
Front: Logic ORs bit atomically (fetch_or, relaxed) as each entity is modified
Back:  Render reads to determine upload set
       Render swaps Front/Back (nanosecond atomic pointer swap) once per frame
```

- Upload set = all entities where Back bit is set; upload their **current state** (not a delta)
- Render holds the slab read lock only during entity state copy (~32µs at 10K dirty entities),
  not during dirty bit coalescing
- Fallback: if `(CurrentLogicFrame - LastGpuUploadFrame) > FrameCount` → full upload all active entities

**When render thread falls behind:** It ORs dirty flags from all intermediate frames before processing:

```
for f in [LastGpuUploadFrame+1 .. CurrentLogicFrame]:
    SIMD OR combinedDirty |= dirtyBits[f % FrameCount]
upload current state for all set bits in combinedDirty
```

This is a SIMD operation on a 12.5KB bit array (100K entities), fits in L1 cache.

## GPU Buffer Design

**5 InstanceBuffers** (for Volatile + Temporal combined entity data):
- Enough depth that the CPU render thread always finds a free buffer without blocking on GPU presentation
- Decouples Logic from VSync: the chain `VSync → GPU holds buffer → CPU render thread blocks →
  holds slab read locks → Logic stalls` is broken by having 5 buffers
- CPU render thread lock on logic slab = only entity state copy duration (~32µs at 10K dirty entities)

GPU interpolation uses its own persistent previous-frame InstanceBuffer. Render thread uploads frame T only
(not T-1). GPU has T-1 from its own last-frame buffer, interpolates in vertex shader.

**Separate static GPU buffer** for static entity data (read-only, uploaded once at initialization). rarely modified.
Render can listen for a staticChange event and kick off an async worker to update the GPU static buffer.

## Thread Access Summary

| Thread           | Reads                                        | Writes                                     |
|------------------|----------------------------------------------|--------------------------------------------|
| Logic (Brain)    | None, works with current state copied to T+1 | Frame T+1 (WriteArray)                     |
| Render (Encoder) | Frame T (pre-seeded WriteArray from last tick) | GPU InstanceBuffer                         |
| GPU              | Previous InstanceBuffer                      | Current InstanceBuffer (for interpolation) |
| Network/Rollback | Historical frames from Temporal slab         | Corrected frame (triggers dirty resim)     |

---

# Rendering Pipeline

## Raw Vulkan Stack

- **volk 1.4.304** — function loader, vendored at `libs/volk/`
- **VMA 3.3.0** — memory allocator, vendored at `libs/vma/`
- **VulkanContext** — instance, device, swapchain, queues, sync primitives
- **VulkanMemory** — VMA allocator lifetime, buffer/image allocation helpers
- **VulkRender** — Encoder thread: GPU upload, compute dispatch, graphics draw

## GPU-Driven Compute Pipeline (Slang shaders in `shaders/`)

Three-pass compute using shared struct header `GpuFrameData.slang`:

1. **predicate.slang** — reads TemporalFlags from `CurrFieldAddrs[0]` (Active bit 31), writes `scan[i] = 0 or 1`
2. **prefix_sum.slang** — Option-B scan: subgroup-level prefix, then one `atomicAdd` per workgroup for
   cross-group compaction. No second dispatch.
3. **scatter.slang** — GPU interpolation (lerp between current and previous InstanceBuffer),
   writes compacted InstanceBuffer SoA + `DrawArgs.instanceCount`

All entity data (including flags) flows through the field slab — 5 PersistentMapped GPU buffers
that cycle independently of the 2 GPU frame-in-flight slots. The render thread copies SoA field
arrays from TemporalComponentCache/VolatileComponentCache into the current slab when a new logic
frame is detected. `GpuFrameData.CurrFieldAddrs[]` and `PrevFieldAddrs[]` point into the current
and previous slabs respectively. Flags are always at field index 0 (`kSemFlags` semantic).

The InstanceBuffer is SoA: field `k` (semantic-1), entity `outIdx` →
`InstancesAddr[k * OutFieldStride + outIdx]`.

## cube.vert / cube.frag

- `cube.vert` reads vertex data via Buffer Device Address (`VerticesAddr`), reads instance SoA via
  `InstancesAddr`, reconstructs TRS matrix from quaternion rotation (nlerp-normalized in shader)
- ViewProj: column-major `float[16]` from C++ → Slang row-major `float4x4` via `M[r][c] = ViewProj[c*4+r]`
- Projection: right-handed, Y-flip (`m[5] = -F`), depth [0,1]
- `cube.frag` reads interpolated color from SoA

## Compute → Graphics Barrier

```
srcStage  = COMPUTE_SHADER_BIT
dstStage  = VERTEX_SHADER_BIT | DRAW_INDIRECT_BIT
srcAccess = SHADER_WRITE
dstAccess = SHADER_READ | INDIRECT_COMMAND_READ
```

---

# Migration Status

## Implemented (2026-03)

- [x] Three-thread architecture (Sentinel/Brain/Encoder)
- [x] Raw Vulkan: VulkanContext, VulkanMemory (volk + VMA), migrated to vk::raii::
- [x] FieldProxy system (Scalar / Wide / WideMask, FieldProxyMask zero-size base)
- [x] Component decomposition into SoA field arrays
- [x] EntityView hydration with zero virtual calls
- [x] TemporalComponentCache dual-buffer (proto-slab)
- [x] Dirty bit tracking (TemporalFlagBits::Active, Dirty)
- [x] Registry dirty bit marking after each chunk update
- [x] LogicThread::PublishCompletedFrame (Vulkan RH perspective + identity view)
- [x] GPU-driven compute pipeline (predicate → prefix_sum → scatter, Slang shaders)
- [x] VulkRender Steps 1–4: clear → indexed cube → GpuFrameData + BDA draw → entity data from TemporalComponentCache
  Live entity data rendering at full rate via Buffer Device Address pipeline
- [x] Tracy profiler integration (3-level: Coarse/Medium/Fine)
- [x] Lock-free job system (MPMC ring buffers, futex-based wake, per-chunk dispatch)
- [x] Core-aware thread pinning (physical cores first, SMT siblings second)
- [x] GameManager CRTP pattern (`TNX_IMPLEMENT_GAME` macro, zero-boilerplate project setup)
- [x] Project-relative INI config (`*Defaults.ini` scanning from source directory)
- [x] Tiered storage partition layout (Cold/Static/Volatile/Temporal with dual-ended arena layout)
- [x] SystemGroup tag on `TNX_TEMPORAL_FIELDS` (drives entity group auto-derivation)
- [x] 5 GPU InstanceBuffers (circular buffer while rGPU compute pipeline is in progress)
- [x] Quaternion-based transforms (TransRot component with nested RotationAccessor, QuatMath library)
- [x] Component decomposition: Translation, Rotation, TransRot (combined), Scale (Volatile/Render)
- [x] Component validation: no vtable + all fields must be FieldProxy (SchemaValidation.h)
- [x] Jolt Physics v5.5.0 integration (JoltJobSystemAdapter, JoltBody cold component,
  FlushPendingBodies/PullActiveTransforms)
- [x] Cold component infrastructure (TNX_REGISTER_FIELDS → CacheTier::None → SoA in chunk, not slab)
- [x] Input buffering (double-buffered input, lock-free polling, event + bitstate querying)
- [x] VecMath / QuatMath / FieldMath libraries

## Designed, Not Yet Implemented

- [ ] **Cumulative dirty bit array wired to GPU upload** (tracking functional, not yet driving upload path)
- [ ] `GetTemporalFieldWritePtr` migrated from Archetype to TemporalComponentCache
- [ ] `TemporalFrameStride` removed from Archetype (duplicated state — call cache->GetFrameStride())
- [ ] **Presentation Reconciler** (Anti-Events, speculative presentation diff)
- [ ] **Fixed-point coordinate system** (Fixed32, SimFloat alias, Jolt bridge)
- [ ] **ConstraintEntity system** (constraint pool, rigid attachment pass, physics root determination)

## Planned (Next Phase)

- [ ] Render pipeline optimization (dirty-bit-driven GPU upload)
- [ ] Frustum culling (SIMD 6-plane test)
- [ ] State-sorted rendering (64-bit sort keys, GPU radix sort)
- [ ] Rollback netcode (Temporal slab rollback + dirty resimulation)

---
