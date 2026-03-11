#pragma once

#include "TransRot.h"
#include "Scale.h"
#include "ColorData.h"
#include "EntityView.h"
#include "Schema.h"
#include "SchemaReflector.h"
#include "FieldProxy.h"
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

	// Lifecycle hooks
	FORCE_INLINE void PrePhysics([[maybe_unused]] double dt)
	{
		const float fdt = static_cast<float>(dt);

		transform.Position += velocity.Vel * fdt;
		transform.PosX     = (transform.PosX > 50.f).Choose(-50.f, transform.PosX);

		velocity.vX *= 0.98f;
		velocity.vY *= 0.99f;

		transform.Rotation.RotateY(fdt * 0.2f);
		transform.Rotation.RotateZ(fdt * 0.4f);
	}
};

template <FieldWidth WIDTH = FieldWidth::Scalar>
class CubeEntity : public BaseCube<CubeEntity, WIDTH>
{
	TNX_REGISTER_SCHEMA(CubeEntity, BaseCube, physBody)

public:
	JoltBody<WIDTH> physBody;

	// Logic
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