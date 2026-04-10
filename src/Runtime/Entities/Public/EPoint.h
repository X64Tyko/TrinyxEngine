#pragma once

#include "EntityView.h"
#include "SchemaReflector.h"
#include "CTransform.h"

template <FieldWidth WIDTH = FieldWidth::Scalar>
class EPoint : public EntityView<EPoint, WIDTH>
{
	TNX_REGISTER_SCHEMA(EPoint, EntityView, Transform)

public:
	CTransform<WIDTH> Transform;

	FORCE_INLINE void PrePhysics([[maybe_unused]] SimFloat dt)
	{
	}
};