#pragma once

#include "CTransform.h"
#include "CScale.h"
#include "CColor.h"
#include "CMeshRef.h"
#include "CJoltBody.h"
#include "EInterpEntity.h"
#include "EntityView.h"
#include "SchemaReflector.h"

// CubeEntity — generic static prop with physics.
// Used for level geometry: floors, walls, cover, etc.
template <FieldWidth WIDTH = FieldWidth::Scalar>
class CubeEntity : public EInterpEntity<CubeEntity, WIDTH>
{
	TNX_REGISTER_SCHEMA(CubeEntity, EInterpEntity, scale, color, mesh, physBody)

	CScale<WIDTH> scale;
	CColor<WIDTH> color;
	CMeshRef<WIDTH> mesh;
	CJoltBody<WIDTH> physBody;
};