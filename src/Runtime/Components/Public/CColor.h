#pragma once
#include <FieldProxy.h>
#include "ComponentView.h"
#include "SchemaReflector.h"
#include "VecMath.h"

// ColorData Component — RGBA color for rendering.
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct CColor : ComponentView<CColor, WIDTH>
{
	TNX_VOLATILE_FIELDS(CColor, Render, R, G, B, A)

	FloatProxy<WIDTH> R;
	FloatProxy<WIDTH> G;
	FloatProxy<WIDTH> B;
	FloatProxy<WIDTH> A;

	Vec4Accessor<WIDTH> Color{R, G, B, A};
};

TNX_REGISTER_COMPONENT(CColor)