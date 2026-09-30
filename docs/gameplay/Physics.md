# Physics

> [← Game Flow](Game-Flow.md) | [Audio →](Audio.md) | [Home](../Home.md)

---

## Jolt Integration Overview

Jolt Physics v5.5.0 runs at a configurable fraction of the logic rate. Default: 512Hz logic / 8 = **64Hz physics**. This ratio is configurable — games needing tighter physics can reduce the divisor. Because the physics step is tied directly to the fixed update rate, it remains deterministic.

**Key principle: Jolt owns physics state.** The ECS does not push state to Jolt every frame. Jolt's integrator advances bodies internally. The ECS only writes to Jolt on explicit overrides (spawn, teleport, impulse, kinematic target). After each step, only **awake bodies** are pulled back into SoA write arrays.

```
PrePhysics → [Override if needed] → Jolt Step → Pull (awake only) → Collision Events → PostPhysics
```

A scene with 50K physics bodies where 200 are moving pays for **200 pulls, not 50K**. Sleeping bodies are the common case.

---

## Physics Tick Timing

Physics entities are pushed into Jolt on `(currentFrame % PhysicsDivizor == 0)`. Transforms are pulled on `(currentFrame % PhysicsDivizor == PhysicsDivizor - 1)`. Between push and pull, the Brain thread acts as a physics worker — it submits Jolt jobs via the `JoltJobSystemAdapter` and steals from the Jolt queue while waiting. This makes full use of the worker pool without a dedicated physics thread.

---

## Overrides: ECS → Jolt (Event-Driven, Not Per-Frame)

The ECS writes to Jolt bodies only when gameplay logic explicitly demands it:

```cpp
JPH::BodyInterface& bi = physicsSystem.GetBodyInterfaceNoLock();

// Spawn: create body with initial state from entity data
bi.CreateAndAddBody(bodyCreationSettings, JPH::EActivation::Activate);

// Teleport: non-physical move
bi.SetPositionAndRotation(bodyID, pos, rot, JPH::EActivation::Activate);

// Gameplay impulse in PrePhysics
bi.AddImpulse(bodyID, JPH::Vec3(ix, iy, iz));

// Kinematic target (animated platforms, elevators)
bi.MoveKinematic(bodyID, targetPos, targetRot, fixedDt);
```

`GetBodyInterfaceNoLock()` is safe here — the Brain thread is the only writer during the physics window.

---

## Pull: Jolt → ECS (Awake Bodies Only)

After each Jolt step, read results back into SoA write arrays for awake bodies only:

```cpp
JPH::BodyIDVector activeIDs;
physicsSystem.GetActiveBodies(JPH::EBodyType::RigidBody, activeIDs);

for (JPH::BodyID bodyID : activeIDs)
{
    uint32_t entityIdx = BodyToEntity[bodyID.GetIndex()];

    JPH::RVec3 pos;  JPH::Quat rot;
    bi.GetPositionAndRotation(bodyID, pos, rot);  // single call, avoids double lock

    WriteArray_PosX[entityIdx] = Fixed32::FromFloat(pos.GetX());
    WriteArray_PosY[entityIdx] = Fixed32::FromFloat(pos.GetY());
    WriteArray_PosZ[entityIdx] = Fixed32::FromFloat(pos.GetZ());

    WriteArray_RotQx[entityIdx] = rot.GetX();
    // ...

    // Mark dirty for GPU upload
    WriteArray_Flags[entityIdx] |= TemporalFlagBits::Dirty;
}
```

**Only transforms are pulled.** Velocities stay in Jolt. Gameplay code that needs velocity queries Jolt directly during `ScalarUpdate` via `BodyInterface::GetLinearVelocity(bodyID)`.

**Implementation detail:** Bodies are stored in AoS. The engine pulls groups of 4 bodies (translation + rotation), SIMD-transposes to SoA format, and writes back to field array positions via the job system.

---

## Collision Events

Jolt fires contact callbacks during `Update()` from worker threads. These are buffered in thread-local rings:

```cpp
class ContactListener : public JPH::ContactListener {
    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, ...) override
    {
        // Thread-local ring — no locks needed
        threadLocalContactBuffer.Push({b1.GetID(), b2.GetID(), manifold, ...});
    }
};
```

After Pull, Brain drains contact buffers via `ProcessContacts()`. Contacts resolve `BodyID → EntityCacheHandle → EntityHandle` and dispatch to callbacks mapped by `BodyID.GetIndex()`.

**Two dispatch paths:**

- **Constructs:** direct typed callbacks auto-bound via concept detection (`HasOnHit`, `HasOnOverlapBegin`, `HasOnOverlapEnd`) during `Construct<T>::Initialize`. Callbacks receive `PhysicsOnHitData` or `PhysicsOverlapData`.

- **Entities (designed, not yet implemented):** `ContactSystem<T>` — one CRTP instance per entity type. Pre-hydrated `EntityView` repoints its FieldProxy cursor to the contacted entity's cache index during `ProcessContacts`. No per-entity allocation, no dynamic View hydration.

---

## Body Lifecycle

```
Entity spawn (with physics component)  → CreateAndAddBody(), store BodyID↔EntityID mapping
Entity destroy                         → DestroyBody()
Entity deactivation (Active=0)         → DeactivateBody()  (skipped in broadphase)
Entity reactivation                    → ActivateBody()
```

`CreateAndAddBody()` is a single call — avoids the create-then-add pattern that requires an extra lock acquisition.

---

## `JoltCharacter` — Character Controller

`JoltCharacter` wraps `JPH::CharacterVirtual` for Construct-driven movement. Completely independent of `CJoltBody` — no Jolt body is created in the ECS.

```cpp
class Player : public Construct<Player>
{
    ConstructView<EPlayer> Body;
    JoltCharacter CharacterController;

    void InitializeViews()
    {
        Body.Initialize(this);
        CharacterController.Initialize(
            GetWorld()->GetPhysics()->GetPhysicsSystem(),
            JPH::RVec3(0, 5, 0),  // spawn position
            0.3f,    // capsule radius
            0.7f);   // capsule half-height
    }

    void PhysicsStep(SimFloat dt)
    {
        CharacterController.Update(
            desiredVelocity,
            gravity,
            dt,
            *GetWorld()->GetPhysics()->GetTempAllocator());

        JPH::RVec3 pos = CharacterController.GetPosition();
        Body.Transform.PosX = SimFloat::FromFloat(pos.GetX());
        Body.Transform.PosY = SimFloat::FromFloat(pos.GetY());
        Body.Transform.PosZ = SimFloat::FromFloat(pos.GetZ());
    }
};
```

`JoltPhysics::GetTempAllocator()` exposes the Jolt temp allocator needed for `ExtendedUpdate`. `JoltLayers.h` defines shared layer constants (Static, Dynamic) used by both `JoltPhysics` and `JoltCharacter`.

---

## Constraint System

Constraints are structural metadata — which body, which anchor, which type. They live in a separate flat AoS pool (not in the temporal slab) because the solver always reads all fields of one constraint together.

> **Status: Designed, not yet implemented.**

```cpp
struct ConstraintEntity
{
    uint32_t       BodyA;        // entity partition index (UINT32_MAX = static anchor)
    uint32_t       BodyB;
    ConstraintType Type;
    uint16_t       BoneA, BoneB; // UINT16_MAX = entity root pivot
    Fixed32        AnchorA[3];   // local-space anchor on BodyA
    Fixed32        AnchorB[3];
    Fixed32        LimitMin[3];  // DOF limits (translation mm or angle units)
    Fixed32        LimitMax[3];
    Fixed32        Stiffness;
    Fixed32        Damping;
};

enum class ConstraintType : uint8_t
{
    Rigid,          // All 6 DOF locked — rigid transform inheritance (weapon to hand)
    RigidWithScale, // Rigid + scale inherited
    PositionOnly,   // Translation locked, rotation free
    Hinge,          // 1 rotational DOF free (door, axle)
    BallSocket,     // 3 rotational DOF free, translation locked (shoulder, hip)
    Prismatic,      // 1 translational DOF free, rotation locked (piston, slider)
    Distance,       // Fixed distance, all rotation free (chain link)
    Spring,         // Distance constraint with stiffness/damping
};
```

**Rigid attachment** (`Type=Rigid`) is a degenerate constraint. The render thread resolves these before GPU upload (world transform inheritance), at no physics solver cost. Entities with a `Type=Rigid` constraint as BodyA do not get independent physics bodies.

**Note on naming:** "Jolt constraints" (Jolt's internal joint system) are a different concept. When Jolt is replaced with a custom solver, Jolt joints disappear but ConstraintEntities remain — the custom solver reads them.

---

## Rollback and Jolt

For rollback, Jolt uses `SaveState`/`RestoreState` via `StateRecorderImpl`. Per-frame snapshots (~7KB for 56 bodies) are stored in a ring buffer after each `PullActiveTransforms`.

When rolling back to frame N: snap to the nearest Jolt execution frame at or before N (e.g., rollback to frame 100 → restore from frame 96 at 8:1 ratio), restore Jolt state, resimulate from there. At most 7 frames of physics approximation in the worst case — acceptable for competitive multiplayer.

**Rebuild-from-slab was tested and rejected.** Restoring only entity positions loses the contact cache, solver warmstarting, and sleep states. The solver reinitializes from scratch and diverges from the original timeline. Snapshot restore is the only correct approach for deterministic resim.

Determinism status (2026-03-29): byte-perfect across 5-12 frame rollbacks with 100k entities + 56 physics bodies.

See [Rollback Netcode](../networking/Rollback-Netcode.md) for the full rollback architecture.

---

## Networked Physics Without Rollback

With rollback disabled, server corrections to physics-driven entities are applied as Jolt teleport overrides, with no
prediction replay. Physics-driven entities an Owner controls will jitter as corrections fight the solver. This is a
known limitation, not a bug to patch around Jolt — see
[Networking Without Rollback](../networking/Overview.md#networking-without-rollback--current-state-and-direction).

---

## Long-Term: Trinyx Physics Solver

Jolt is a temporary backend. The long-term direction is a Trinyx physics solver built on the engine's own data layout:
solver state lives in slab fields rather than a separate physics world, so snapshot, rollback, partial re-simulation of
predicted entities, and GPU upload all work on the same data. Full client-side prediction for physics-driven entities
waits on this solver. The constraint system (`ConstraintEntity`) is already designed to be solver-agnostic so it carries
over unchanged.
