#pragma once
#include <FieldProxy.h>
#include "ComponentView.h"
#include "SchemaReflector.h"
#include "VecMath.h"

// Translation Component — position only.
// Use when an entity needs position but not rotation (e.g. particles, UI elements).
// Most entities should prefer TransRot instead.
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct CTranslation : ComponentView<CTranslation, WIDTH>
{
	TNX_TEMPORAL_FIELDS(CTranslation, Physics, PosX, PosY, PosZ)

	FloatProxy<WIDTH> PosX;
	FloatProxy<WIDTH> PosY;
	FloatProxy<WIDTH> PosZ;

	Vec3Accessor<WIDTH> Position{PosX, PosY, PosZ};
};

TNX_REGISTER_COMPONENT(CTranslation)