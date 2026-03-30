#pragma once
#include "ComponentView.h"
#include "SchemaReflector.h"

// Shape type constants for the Shape field.
namespace JoltShapeType
{
	constexpr uint32_t Box     = 0; // HalfExtentX/Y/Z = half-dimensions
	constexpr uint32_t Sphere  = 1; // HalfExtentX = radius; Y/Z unused
	constexpr uint32_t Capsule = 2; // HalfExtentX = radius; HalfExtentY = half-height; Z unused
}

// Motion type constants for the Motion field.
namespace JoltMotion
{
	constexpr uint32_t Static    = 0; // Immovable (floors, walls)
	constexpr uint32_t Kinematic = 1; // Moved by code, not by forces
	constexpr uint32_t Dynamic   = 2; // Fully simulated by Jolt
}

// JoltBody component — declares that an entity participates in Jolt physics.
//
// Including this component in an entity's schema is all that's needed.
// Set shape/mass/motion during entity initialization, then call
// JoltPhysics::FlushPendingBodies() to batch-create bodies in Jolt.
//
// Jolt owns the authoritative physics state (position, rotation, velocity).
// The engine pulls transforms from active (awake) bodies after each Step().
// The ECS only overrides Jolt on spawn, teleport, or explicit impulse.
//
// Volatile: SoA arrays live in the volatile slab (Physics partition), enabling
// direct slab iteration for FlushPendingBodies without archetype/chunk indirection.
// Body settings are write-once config but stored in the slab so physics can
// scan the contiguous DUAL+PHYS region as a single dense pass.
// BodyID ↔ EntityIndex mapping lives in JoltPhysics as two lookup arrays.
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct JoltBody : ComponentView<JoltBody, WIDTH>
{
	TNX_VOLATILE_FIELDS(JoltBody, Physics, Shape, HalfExtentX, HalfExtentY, HalfExtentZ,
						Motion, Mass, Friction, Restitution)

	// Shape geometry
	UIntProxy<WIDTH> Shape;        // JoltShapeType:: constant
	FloatProxy<WIDTH> HalfExtentX; // Box: half X, Sphere/Capsule: radius
	FloatProxy<WIDTH> HalfExtentY; // Box: half Y, Capsule: half height
	FloatProxy<WIDTH> HalfExtentZ; // Box: half Z

	// Physics behavior
	UIntProxy<WIDTH> Motion; // JoltMotion:: constant
	FloatProxy<WIDTH> Mass;
	FloatProxy<WIDTH> Friction;
	FloatProxy<WIDTH> Restitution;
};

TNX_REGISTER_COMPONENT(JoltBody)