#pragma once

#include "Transform.h"
#include "ColorData.h"
#include "EntityView.h"
#include "Schema.h"
#include "SchemaReflector.h"
#include "FieldProxy.h"

template <template <FieldWidth> class T, FieldWidth WIDTH = FieldWidth::Scalar>
class BaseCube : public EntityView<T, WIDTH>
{
	STRIGID_REGISTER_SUPER_SCHEMA(BaseCube, EntityView, transform, velocity, color)

	Transform<WIDTH> transform;
	Velocity<WIDTH> velocity;
	ColorData<WIDTH> color;

	// Lifecycle hooks
	FORCE_INLINE void PrePhysics([[maybe_unused]] double dt)
	{
		//constexpr float TWO_PI = 6.283185307179586f;

		// Now we emulate less than ideal assignment and operations in a fixed update.
		transform.PositionX += static_cast<float>(dt) * velocity.vX;
		transform.PositionX = (transform.PositionX > 50.f).Choose(-50.f, transform.PositionX);

		velocity.vX *= static_cast<float>(0.98f);
		velocity.vY *= static_cast<float>(0.99f);

		transform.RotationY += static_cast<float>(dt) * 0.2f;

		transform.RotationZ += static_cast<float>(dt) * 0.4f;
	}
};

template <FieldWidth WIDTH = FieldWidth::Scalar>
class CubeEntity : public BaseCube<CubeEntity, WIDTH>
{
	STRIGID_REGISTER_SCHEMA(CubeEntity, BaseCube)
};

template <FieldWidth WIDTH = FieldWidth::Scalar>
class SuperCube : public BaseCube<SuperCube, WIDTH>
{
	STRIGID_REGISTER_SCHEMA(SuperCube, BaseCube)
	using Base::transform;
	using Base::velocity;
	using Base::color;

public:
	// Logic
	FORCE_INLINE void ScalarUpdate([[maybe_unused]] double dt)
	{
		color.R = (color.R + (static_cast<float>(dt) * 0.5f) > 1.f) ? 0.f : color.R + (static_cast<float>(dt) * 0.5f);
		color.G = (color.G + (static_cast<float>(dt) * 0.3f) > 1.f) ? 0.f : color.G + (static_cast<float>(dt) * 0.3f);
		color.B = (color.B + static_cast<float>(dt) > 1.f) ? 0.f : color.B + static_cast<float>(dt);
	}
};
