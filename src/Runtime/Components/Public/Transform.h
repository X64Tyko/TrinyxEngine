#pragma once

#include "ComponentView.h"
#include "SchemaReflector.h"

// Transform Component - Position, Rotation, Scale
// Temporal component: stored in TemporalComponentCache for history/rollback
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct Transform : ComponentView<Transform, WIDTH>
{
	STRIGID_TEMPORAL_FIELDS(Transform, PositionX, PositionY, PositionZ, RotationX, RotationY, RotationZ, ScaleX, ScaleY, ScaleZ)

	FloatProxy<WIDTH> PositionX;
	FloatProxy<WIDTH> PositionY;
	FloatProxy<WIDTH> PositionZ;

	FloatProxy<WIDTH> RotationX; // Euler angles for now
	FloatProxy<WIDTH> RotationY;
	FloatProxy<WIDTH> RotationZ;

	FloatProxy<WIDTH> ScaleX;
	FloatProxy<WIDTH> ScaleY;
	FloatProxy<WIDTH> ScaleZ;
};

STRIGID_REGISTER_COMPONENT(Transform)