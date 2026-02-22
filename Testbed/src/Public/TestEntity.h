#pragma once

#include "Transform.h"
#include "Velocity.h"
#include "ColorData.h"
#include "EntityView.h"
#include "Schema.h"
#include "SchemaReflector.h"

// Simple test struct (will be replaced with real components in Week 4)
template<bool MASK = false>
class TestEntity : public EntityView<TestEntity>
{
    STRIGID_REGISTER_SCHEMA(TestEntity, EntityView, Transform, Velocity)
    
    Transform<MASK> Transform;
    Velocity<MASK> Velocity;

    void Update([[maybe_unused]] double dt)
    {
    }
};
