#pragma once
#include "ComponentView.h"
#include "SchemaReflector.h"

// BoundingVolume component — axis-aligned bounding box in world space.
// Updated each frame from Transform + the entity's constrained Collider child(ren).
// Consumed by the GPU predicate pass for frustum culling.
// Volatile: derived per-frame, no rollback needed. Render partition group (Render).
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct BoundingVolume : ComponentView<BoundingVolume, WIDTH>
{
	TNX_VOLATILE_FIELDS(BoundingVolume, Physics, MinX, MinY, MinZ, MaxX, MaxY, MaxZ)

	FloatProxy<WIDTH> MinX;
	FloatProxy<WIDTH> MinY;
	FloatProxy<WIDTH> MinZ;

	FloatProxy<WIDTH> MaxX;
	FloatProxy<WIDTH> MaxY;
	FloatProxy<WIDTH> MaxZ;
};

TNX_REGISTER_COMPONENT(BoundingVolume)