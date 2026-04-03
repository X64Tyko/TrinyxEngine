#pragma once

#include "CColor.h"
#include "EntityView.h"
#include "CJoltBody.h"
#include "CMeshRef.h"
#include "CScale.h"
#include "SchemaReflector.h"
#include "CTransform.h"

template <FieldWidth WIDTH = FieldWidth::Scalar>
class EInstanced : public EntityView<EInstanced, WIDTH>
{
	TNX_REGISTER_SCHEMA(EInstanced, EntityView, Transform, Scale, Color, Mesh, PhysBody)

public:
	CTransform<WIDTH> Transform;
	CScale<WIDTH> Scale;
	CColor<WIDTH> Color;
	CMeshRef<WIDTH> Mesh;
	CJoltBody<WIDTH> PhysBody;
};
