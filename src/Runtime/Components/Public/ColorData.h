#pragma once
#include <FieldProxy.h>
#include "ComponentView.h"
#include "SchemaReflector.h"

// ColorData Component - RGBA color for rendering
// Aligned to 32 bytes for GPU upload
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct ColorData : ComponentView<ColorData, WIDTH>
{
	STRIGID_TEMPORAL_FIELDS(ColorData, R, G, B, A)

	FloatProxy<WIDTH> R;
	FloatProxy<WIDTH> G;
	FloatProxy<WIDTH> B;
	FloatProxy<WIDTH> A;
};

STRIGID_REGISTER_COMPONENT(ColorData)