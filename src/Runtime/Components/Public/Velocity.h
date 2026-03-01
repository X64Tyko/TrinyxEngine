#pragma once
#include <FieldProxy.h>
#include "ComponentView.h"
#include "SchemaReflector.h"

// Velocity Component - Linear velocity for movement
// Aligned to 16 bytes for SIMD operations
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct Velocity : ComponentView<Velocity, WIDTH>
{
	STRIGID_TEMPORAL_FIELDS(Velocity, vX, vY, vZ)

	FloatProxy<WIDTH> vX;
	FloatProxy<WIDTH> vY;
	FloatProxy<WIDTH> vZ;
};

STRIGID_REGISTER_COMPONENT(Velocity)