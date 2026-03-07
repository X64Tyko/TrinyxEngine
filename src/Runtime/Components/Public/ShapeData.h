#pragma once
#include "ComponentView.h"
#include "SchemaReflector.h"

// Shape types for ColliderShape field.
enum class ColliderShape : uint32_t
{
	Box     = 0, // HalfExtentX/Y/Z = half-dimensions
	Sphere  = 1, // HalfExtentX = radius; Y/Z unused
	Capsule = 2, // HalfExtentX = radius; HalfExtentY = half-height; Z unused
};

// ShapeData component — collision geometry and physical material parameters.
// Carried by Collider entities (which are constrained to their owning physics entity
// via the ConstraintPool). Not placed on the physics entity itself.
//
// Cold: stored in archetype chunks (AoS). Shape geometry is set at spawn and
// does not change per-frame; no SoA allocation or rollback needed.
//
// Cold component interface: plain POD fields, no FieldProxy.
// Bind() is a no-op; chunk access goes through Archetype::GetComponentArray.
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct ShapeData : ComponentView<ShapeData, WIDTH>
{
	UIntProxy<WIDTH> Shape        = static_cast<uint32_t>(ColliderShape::Box);
	FloatProxy<WIDTH> HalfExtentX = 0.5f;
	FloatProxy<WIDTH> HalfExtentY = 0.5f;
	FloatProxy<WIDTH> HalfExtentZ = 0.5f;
	FloatProxy<WIDTH> Mass        = 1.0f;
	FloatProxy<WIDTH> Restitution = 0.3f; // Bounciness [0, 1]
	FloatProxy<WIDTH> Friction    = 0.5f; // Surface friction [0, 1]

	TNX_VOLATILE_FIELDS(ShapeData, Physics, Shape, HalfExtentsX, HalfExtentsY, HalfExtentsZ, Mass, Restitution, Friction)
};

TNX_REGISTER_COMPONENT(ShapeData)