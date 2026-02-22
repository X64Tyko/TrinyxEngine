#pragma once
#include <FieldProxy.h>
#include "ComponentView.h"
#include "SchemaReflector.h"

// ColorData Component - RGBA color for rendering
// Aligned to 32 bytes for GPU upload
template<bool MASK = false>
struct ColorData : ComponentView<ColorData>
{
    STRIGID_REGISTER_FIELDS(ColorData, R, G, B, A)
    
    FloatProxy<MASK> R;
    FloatProxy<MASK> G;
    FloatProxy<MASK> B;
    FloatProxy<MASK> A;
};

STRIGID_REGISTER_COMPONENT(ColorData)