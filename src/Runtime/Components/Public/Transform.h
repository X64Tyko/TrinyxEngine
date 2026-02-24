#pragma once

#include "ComponentView.h"
#include "SchemaReflector.h"

// Transform Component - Position, Rotation, Scale
// Temporal component: stored in TemporalComponentCache for history/rollback
template <bool MASK = false>
struct Transform : ComponentView<Transform, MASK>
{
    STRIGID_TEMPORAL_FIELDS(Transform, PositionX, PositionY, PositionZ, RotationX, RotationY, RotationZ, ScaleX, ScaleY, ScaleZ)
    
    FloatProxy<MASK> PositionX;
    FloatProxy<MASK> PositionY;
    FloatProxy<MASK> PositionZ;

    FloatProxy<MASK> RotationX; // Euler angles for now
    FloatProxy<MASK> RotationY;
    FloatProxy<MASK> RotationZ;

    FloatProxy<MASK> ScaleX;
    FloatProxy<MASK> ScaleY;
    FloatProxy<MASK> ScaleZ;
};

STRIGID_REGISTER_COMPONENT(Transform)
