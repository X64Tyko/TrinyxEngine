#pragma once

#include "CTransform.h"
#include "CColor.h"
#include "CRigidBody.h"
#include "CForces.h"
#include "EntityView.h"
#include "SchemaReflector.h"

// PhysicsEntity — exercises the full pre-Jolt physics data pipeline.
// Carries RigidBody (Temporal) and Forces (Volatile) alongside Transform.
// PrePhysics: accumulates gravity into Forces, integrates velocity into position.
// PostPhysics: zeroes Forces so each tick starts clean.
// Partition: Dual (has both Physics and Render components).
template <FieldWidth WIDTH = FieldWidth::Scalar>
class PhysicsEntity : public EntityView<PhysicsEntity, WIDTH>
{
	TNX_REGISTER_SCHEMA(PhysicsEntity, EntityView, transform, body, forces, color)

public:
	CTransform<WIDTH> transform;
	CRigidBody<WIDTH> body;
	CForces<WIDTH> forces;
	CColor<WIDTH> color;

	static constexpr float Gravity = -9.81f;

	FORCE_INLINE void PrePhysics(SimFloat dt)
	{
		// Accumulate gravity
		forces.ForceY += body.VelY * 0.0f; // placeholder — Jolt will own this path
		body.VelY     += Gravity * dt;

		// Integrate velocity into position
		transform.PosX += body.VelX * dt;
		transform.PosY += body.VelY * dt;
		transform.PosZ += body.VelZ * dt;

		// Simple floor bounce at Y = -50
		body.VelY      = (transform.PosY < -50.0f).Choose(-body.VelY * 0.7f, body.VelY);
		transform.PosY = (transform.PosY < -50.0f).Choose(-50.0f, transform.PosY);

		// Tint toward red as downward velocity increases
		const float speed = body.VelY * -0.02f;
		color.R           = (speed > 1.0f).Choose(1.0f, speed);
		color.G           = 1.0f - color.R;
	}

	FORCE_INLINE void PostPhysics([[maybe_unused]] SimFloat dt)
	{
		forces.ForceX  = 0.0f;
		forces.ForceY  = 0.0f;
		forces.ForceZ  = 0.0f;
		forces.TorqueX = 0.0f;
		forces.TorqueY = 0.0f;
		forces.TorqueZ = 0.0f;
	}
};

TNX_REGISTER_ENTITY(PhysicsEntity)