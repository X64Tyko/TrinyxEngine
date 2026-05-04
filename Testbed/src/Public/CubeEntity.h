#pragma once

#include "CTransform.h"
#include "CVelocity.h"
#include "CScale.h"
#include "CColor.h"
#include "EntityView.h"
#include "Schema.h"
#include "SchemaReflector.h"
#include "FieldProxy.h"
#include "CJoltBody.h"

template <template <FieldWidth> class Derived, FieldWidth WIDTH = FieldWidth::Scalar>
class BaseCube : public EntityView<Derived, WIDTH>
{
	TNX_REGISTER_SUPER_SCHEMA(BaseCube, EntityView, transform, velocity, scale, color)
public:
	CTransform<WIDTH> transform;
	CVelocity<WIDTH> velocity;
	CScale<WIDTH> scale;
	CColor<WIDTH> color;

	// Lifecycle hooks
	FORCE_INLINE void PrePhysics([[maybe_unused]] SimFloat dt)
	{
		transform.Position += velocity.Vel * dt;
		transform.PosX     = (transform.PosX > SimFloat(50.f)).Choose(SimFloat(-50.f), transform.PosX);

		velocity.vX *= SimFloat(0.98f);
		velocity.vY *= SimFloat(0.99f);

		transform.Rotation.RotateY(dt * SimFloat(0.2f));
		transform.Rotation.RotateZ(dt * SimFloat(0.4f));
	}
};






template <FieldWidth WIDTH = FieldWidth::Scalar>
class CubeEntity : public BaseCube<CubeEntity, WIDTH>
{
	TNX_REGISTER_SCHEMA(CubeEntity, BaseCube, physBody)

public:
	CJoltBody<WIDTH> physBody;

	// Logic
	FORCE_INLINE void PrePhysics([[maybe_unused]] SimFloat dt)
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
	FORCE_INLINE void ScalarUpdate([[maybe_unused]] SimFloat dt)
	{
		color.R = (color.R + (dt * SimFloat(0.5f)) > SimFloat(1.f)) ? SimFloat(0.f) : color.R + (dt * SimFloat(0.5f));
		color.B = ((color.B + dt) > SimFloat(1.f)) ? SimFloat(0.f) : color.B + dt;
	}
};