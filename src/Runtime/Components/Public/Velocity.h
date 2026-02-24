#pragma once
#include <FieldProxy.h>
#include "ComponentView.h"
#include "SchemaReflector.h"

// Velocity Component - Linear velocity for movement
// Aligned to 16 bytes for SIMD operations
template<bool MASK = false>
struct Velocity : ComponentView<Velocity>
{
    STRIGID_TEMPORAL_FIELDS(Velocity, vX, vY, vZ)
    
    FloatProxy<MASK> vX;
    FloatProxy<MASK> vY;
    FloatProxy<MASK> vZ;
};

STRIGID_REGISTER_COMPONENT(Velocity)