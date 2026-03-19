#pragma once

#include "TransRot.h"
#include "Scale.h"
#include "ColorData.h"
#include "MeshRef.h"
#include "JoltBody.h"
#include "EntityView.h"
#include "SchemaReflector.h"

// MeshProp — entity with mesh reference + physics.
// Render partition: DUAL (physics + render).
// Drop as a prefab with a specific MeshRef.MeshID to render imported meshes.
template <FieldWidth WIDTH = FieldWidth::Scalar>
class MeshProp : public EntityView<MeshProp, WIDTH>
{
	TNX_REGISTER_SCHEMA(MeshProp, EntityView, transform, scale, color, mesh, physBody)

public:
	TransRot<WIDTH> transform;
	Scale<WIDTH> scale;
	ColorData<WIDTH> color;
	MeshRef<WIDTH> mesh;
	JoltBody<WIDTH> physBody;

	FORCE_INLINE void PrePhysics([[maybe_unused]] double dt)
	{
	}
};
