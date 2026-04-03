#pragma once

#include "CTransform.h"
#include "CScale.h"
#include "CColor.h"
#include "CMeshRef.h"
#include "EntityView.h"
#include "SchemaReflector.h"

// AmbientProp — render-only volatile entity. No physics, no RigidBody.
// Exercises the Render partition path: only the render thread and GPU
// predicate/scatter passes touch these entities. Physics solver skips them entirely.
// Good for testing frustum culling throughput once the predicate pass is live.
// Partition: Render.
template <FieldWidth WIDTH = FieldWidth::Scalar>
class AmbientProp : public EntityView<AmbientProp, WIDTH>
{
	TNX_REGISTER_SCHEMA(AmbientProp, EntityView, transform, scale, color, mesh)

public:
	CTransform<WIDTH> transform;
	CScale<WIDTH> scale;
	CColor<WIDTH> color;
	CMeshRef<WIDTH> mesh;

	FORCE_INLINE void PrePhysics(SimFloat dt)
	{
		// Gentle idle rotation
		transform.Rotation.RotateY(dt * 0.3f);
		transform.Rotation.RotateZ(dt * 0.1f);

		// Slow color breathe
		color.R = (color.R + dt * 0.15f > 1.0f).Choose(0.0f, color.R + dt * 0.15f);
	}
};

TNX_REGISTER_ENTITY(AmbientProp)