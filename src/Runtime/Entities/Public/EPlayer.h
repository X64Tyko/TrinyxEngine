#pragma once

#include "CColor.h"
#include "EntityView.h"
#include "CMeshRef.h"
#include "CScale.h"
#include "SchemaReflector.h"
#include "CTransform.h"
#include "CVelocity.h"

template <FieldWidth WIDTH = FieldWidth::Scalar>
class EPlayer : public EntityView<EPlayer, WIDTH>
{
	TNX_REGISTER_SCHEMA(EPlayer, EntityView, Transform, Velocity, Scale, Color, Mesh)

public:
	CTransform<WIDTH> Transform;
	CVelocity<WIDTH> Velocity;
	CScale<WIDTH> Scale;
	CColor<WIDTH> Color;
	CMeshRef<WIDTH> Mesh;
};
