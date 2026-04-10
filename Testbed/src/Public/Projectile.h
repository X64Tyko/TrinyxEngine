#pragma once

#include "CTransform.h"
#include "CRigidBody.h"
#include "CColor.h"
#include "EntityView.h"
#include "SchemaReflector.h"
#include "FieldMath.h"

// Projectile — minimal-field high-count entity for AVX2 wide path throughput testing.
// Spawned in large batches (10k–100k). Three components, fast PrePhysics, no branchy logic.
// Despawn is deferred — when PositionZ exceeds the far plane the entity is flagged inactive
// via TemporalFlags::Active, leaving the slot free for the next batch.
// Partition: Dual (Transform + RigidBody = Physics; ColorData = Render).
template <FieldWidth WIDTH = FieldWidth::Scalar>
class Projectile : public EntityView<Projectile, WIDTH>
{
	TNX_REGISTER_SCHEMA(Projectile, EntityView, transform, body, color)

public:
	CTransform<WIDTH> transform;
	CRigidBody<WIDTH> body;
	CColor<WIDTH> color;

	FORCE_INLINE void PrePhysics(SimFloat dt)
	{
		transform.PosX += body.VelX * dt;
		transform.PosY += body.VelY * dt;
		transform.PosZ += body.VelZ * dt;

		// Simple drag
		body.VelX *= 0.999f;
		body.VelY *= 0.999f;
		body.VelZ *= 0.999f;

		// Fade alpha as the projectile slows — tests color write path
		color.A = body.VelZ * (body.VelZ * 0.001f);
		color.A = FieldMath::Clamp(color.A, 0.0f, 1.0f);
	}
};

TNX_REGISTER_ENTITY(Projectile)