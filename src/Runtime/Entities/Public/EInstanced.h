#pragma once

#include "CColor.h"
#include "EntityView.h"
#include "CJoltBody.h"
#include "CMeshRef.h"
#include "CScale.h"
#include "SchemaReflector.h"
#include "EInterpEntity.h"

template <FieldWidth WIDTH = FieldWidth::Scalar>
class EInstanced : public EInterpEntity<EInstanced, WIDTH>
{
	TNX_REGISTER_SCHEMA(EInstanced, EInterpEntity, Scale, Color, Mesh, PhysBody)

	CScale<WIDTH> Scale;
	CColor<WIDTH> Color;
	CMeshRef<WIDTH> Mesh;
	CJoltBody<WIDTH> PhysBody;
};
