#pragma once

#include "Transform.h"
#include "Velocity.h"
#include "EntityView.h"
#include "SchemaReflector.h"

// Simple test struct (will be replaced with real components in Week 4)
template<FieldWidth WIDTH = FieldWidth::Scalar>
class TestEntity : public EntityView<TestEntity, WIDTH>
{
	static constexpr uint32_t kEntitiesPerChunk = 16;
	TNX_REGISTER_SCHEMA(TestEntity, EntityView, transform, velocity)

public:
	Transform<WIDTH> transform;
    Velocity<WIDTH> velocity;

	FORCE_INLINE void PrePhysics([[maybe_unused]] double dt)
	{
		transform.PositionX += velocity.vX;
	}
};
