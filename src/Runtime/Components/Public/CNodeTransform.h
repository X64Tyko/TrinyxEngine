#pragma once

#include "ComponentView.h"
#include "SchemaReflector.h"
#include "VecMath.h"
#include "QuatMath.h"

// NodeTransform — cold position + quaternion for editor-placed camera nodes.
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct CNodeTransform : ComponentView<CNodeTransform, WIDTH>
{
	TNX_REGISTER_FIELDS(CNodeTransform, PosX, PosY, PosZ, RotQx, RotQy, RotQz, RotQw)

	FloatProxy<WIDTH> PosX;
	FloatProxy<WIDTH> PosY;
	FloatProxy<WIDTH> PosZ;

	FloatProxy<WIDTH> RotQx;
	FloatProxy<WIDTH> RotQy;
	FloatProxy<WIDTH> RotQz;
	FloatProxy<WIDTH> RotQw;

	Vec3Accessor<WIDTH> Position{PosX, PosY, PosZ};
	QuatAccessor<WIDTH> Rotation{RotQx, RotQy, RotQz, RotQw};
};

TNX_REGISTER_COMPONENT(CNodeTransform)
