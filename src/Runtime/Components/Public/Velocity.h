#pragma once
#include <FieldProxy.h>
#include "ComponentView.h"
#include "SchemaReflector.h"
#include "VecMath.h"

// Velocity Component — Linear velocity for movement.
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct Velocity : ComponentView<Velocity, WIDTH>
{
	TNX_TEMPORAL_FIELDS(Velocity, Physics, vX, vY, vZ)

	FloatProxy<WIDTH> vX;
	FloatProxy<WIDTH> vY;
	FloatProxy<WIDTH> vZ;

	Vec3Accessor<WIDTH> Vel{vX, vY, vZ};
};

TNX_REGISTER_COMPONENT(Velocity)