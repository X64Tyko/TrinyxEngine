#pragma once

#include "CTransform.h"
#include "CScale.h"
#include "CColor.h"
#include "CMeshRef.h"
#include "CJoltBody.h"
#include "EntityView.h"
#include "SchemaReflector.h"

// CubeEntity — generic static prop with physics.
// Used for level geometry: floors, walls, cover, etc.
template <FieldWidth WIDTH = FieldWidth::Scalar>
class CubeEntity : public EntityView<CubeEntity, WIDTH>
{
	TNX_REGISTER_SCHEMA(CubeEntity, EntityView, transform, scale, color, mesh, physBody)

public:
	CTransform<WIDTH> transform;
	CScale<WIDTH> scale;
	CColor<WIDTH> color;
	CMeshRef<WIDTH> mesh;
	CJoltBody<WIDTH> physBody;

	FORCE_INLINE void PrePhysics([[maybe_unused]] SimFloat dt)
	{
	}
};
