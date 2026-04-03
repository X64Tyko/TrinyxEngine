#pragma once

#include "CTransform.h"
#include "CVelocity.h"
#include "EntityView.h"
#include "SchemaReflector.h"

// Simple test struct (will be replaced with real components in Week 4)
template <FieldWidth WIDTH = FieldWidth::Scalar>
class TestEntity : public EntityView<TestEntity, WIDTH>
{
	static constexpr uint32_t EntitiesPerChunk = 16;
	TNX_REGISTER_SCHEMA(TestEntity, EntityView, transform, velocity)

public:
	CTransform<WIDTH> transform;
	CVelocity<WIDTH> velocity;

	FORCE_INLINE void PrePhysics([[maybe_unused]] SimFloat dt)
	{
		transform.PosX += velocity.vX;
	}
};