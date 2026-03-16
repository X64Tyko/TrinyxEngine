#pragma once

#include "TransRot.h"
#include "Velocity.h"
#include "Scale.h"
#include "ColorData.h"
#include "EntityView.h"
#include "Schema.h"
#include "SchemaReflector.h"
#include "JoltBody.h"

template <template <FieldWidth> class T, FieldWidth WIDTH = FieldWidth::Scalar>
class BaseCube : public EntityView<T, WIDTH>
{
	TNX_REGISTER_SUPER_SCHEMA(BaseCube, EntityView, transform, velocity, scale, color)

public:
	TransRot<WIDTH> transform;
	Velocity<WIDTH> velocity;
	Scale<WIDTH> scale;
	ColorData<WIDTH> color;

	FORCE_INLINE void PrePhysics([[maybe_unused]] double dt)
	{
	}
};

template <FieldWidth WIDTH = FieldWidth::Scalar>
class CubeEntity : public BaseCube<CubeEntity, WIDTH>
{
	TNX_REGISTER_SCHEMA(CubeEntity, BaseCube, physBody)

public:
	JoltBody<WIDTH> physBody;

	FORCE_INLINE void PrePhysics([[maybe_unused]] double dt)
	{
	}
};

template <FieldWidth WIDTH = FieldWidth::Scalar>
class SuperCube : public BaseCube<SuperCube, WIDTH>
{
	TNX_REGISTER_SCHEMA(SuperCube, BaseCube)

public:
	using Base::transform;
	using Base::velocity;
	using Base::scale;
	using Base::color;

	// Logic
	FORCE_INLINE void ScalarUpdate([[maybe_unused]] double dt)
	{
		color.R = (color.R + (static_cast<float>(dt) * 0.5f) > 1.f) ? 0.f : color.R + (static_cast<float>(dt) * 0.5f);
		color.B = ((color.B + static_cast<float>(dt)) > 1.f) ? 0.f : color.B + static_cast<float>(dt);
	}
};
