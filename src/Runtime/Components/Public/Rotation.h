#pragma once
#include <FieldProxy.h>
#include "ComponentView.h"
#include "SchemaReflector.h"
#include "QuatMath.h"

// Rotation Component — quaternion only.
// Use when an entity needs rotation but not position (rare — turrets, joints).
// Most entities should prefer TransRot instead.
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct Rotation : ComponentView<Rotation, WIDTH>
{
	TNX_TEMPORAL_FIELDS(Rotation, Physics, RotQx, RotQy, RotQz, RotQw)

	FloatProxy<WIDTH> RotQx;
	FloatProxy<WIDTH> RotQy;
	FloatProxy<WIDTH> RotQz;
	FloatProxy<WIDTH> RotQw;

	QuatAccessor<WIDTH> Quat{RotQx, RotQy, RotQz, RotQw};
};

TNX_REGISTER_COMPONENT(Rotation)