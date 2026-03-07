#pragma once

#include "Transform.h"
#include "RigidBody.h"
#include "ColorData.h"
#include "EntityView.h"
#include "SchemaReflector.h"

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
	Transform<WIDTH> transform;
	RigidBody<WIDTH> body;
	ColorData<WIDTH> color;

	FORCE_INLINE void PrePhysics(double dt)
	{
		const float fdt = static_cast<float>(dt);

		transform.PositionX += body.VelX * fdt;
		transform.PositionY += body.VelY * fdt;
		transform.PositionZ += body.VelZ * fdt;

		// Simple drag
		body.VelX *= 0.999f;
		body.VelY *= 0.999f;
		body.VelZ *= 0.999f;

		// Fade alpha as the projectile slows — tests color write path
		const float speed = body.VelZ * body.VelZ;
		color.A           = (speed * 0.001f > 1.0f).Choose(1.0f, speed * 0.001f);
	}
};

TNX_REGISTER_ENTITY(Projectile)