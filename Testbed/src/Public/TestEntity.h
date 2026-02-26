#pragma once

#include "Transform.h"
#include "Velocity.h"
#include "EntityView.h"
#include "SchemaReflector.h"

// Simple test struct (will be replaced with real components in Week 4)
template<FieldWidth WIDTH = FieldWidth::Scalar>
class TestEntity : public EntityView<TestEntity>
{
    STRIGID_REGISTER_SCHEMA(TestEntity, EntityView, transform, velocity)

    Transform<WIDTH> transform;
    Velocity<WIDTH> velocity;

    FORCE_INLINE void Update([[maybe_unused]] double dt)
    {
        //LOG_INFO_F("Entity flags: %ui", Flags.Flags);
    }
};
