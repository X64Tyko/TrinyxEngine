#pragma once
#include <FieldProxy.h>
#include "ComponentView.h"
#include "SchemaReflector.h"
#include "VecMath.h"

// Scale Component — per-axis scale, stored as Volatile/Render.
// Set once at spawn, rarely mutated. Kept out of the temporal slab
// to avoid burning 3 SoA fields × N history frames on data that
// almost never changes.
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct Scale : ComponentView<Scale, WIDTH>
{
	TNX_VOLATILE_FIELDS(Scale, Render, ScaleX, ScaleY, ScaleZ)

	FloatProxy<WIDTH> ScaleX;
	FloatProxy<WIDTH> ScaleY;
	FloatProxy<WIDTH> ScaleZ;

	Vec3Accessor<WIDTH> Value{ScaleX, ScaleY, ScaleZ};
};

TNX_REGISTER_COMPONENT(Scale)