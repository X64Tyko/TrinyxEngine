#pragma once

#include "CColor.h"
#include "EntityView.h"
#include "CMeshRef.h"
#include "CScale.h"
#include "SchemaReflector.h"
#include "CVelocity.h"
#include "EInterpEntity.h"

template <FieldWidth WIDTH = FieldWidth::Scalar>
class EPlayer : public EInterpEntity<EPlayer, WIDTH>
{
	TNX_REGISTER_SCHEMA(EPlayer, EInterpEntity, Velocity, Scale, Color, Mesh)

	CVelocity<WIDTH> Velocity;
	CScale<WIDTH> Scale;
	CColor<WIDTH> Color;
	CMeshRef<WIDTH> Mesh;
	
	void Initialize()
	{
		EInterpEntity<EPlayer, WIDTH>::Initialize();
		this->VisTransform.VisBlend = SimFloat(0.8f); // some default lerp for characters
	}
};
