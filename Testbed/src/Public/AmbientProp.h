#pragma once

#include "TransRot.h"
#include "Scale.h"
#include "ColorData.h"
#include "MeshRef.h"
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
	TransRot<WIDTH> transform;
	Scale<WIDTH> scale;
	ColorData<WIDTH> color;
	MeshRef<WIDTH> mesh;

	FORCE_INLINE void PrePhysics(double dt)
	{
		const float fdt = static_cast<float>(dt);

		// Gentle idle rotation
		transform.Rotation.RotateY(fdt * 0.3f);
		transform.Rotation.RotateZ(fdt * 0.1f);

		// Slow color breathe
		color.R = (color.R + fdt * 0.15f > 1.0f).Choose(0.0f, color.R + fdt * 0.15f);
	}
};

TNX_REGISTER_ENTITY(AmbientProp)