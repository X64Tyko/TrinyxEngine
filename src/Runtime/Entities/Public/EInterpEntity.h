#pragma once
#include "CTransform.h"
#include "CVisualTransform.h"
#include "EntityView.h"
#include "Types.h"


template <template <FieldWidth> class Derived, FieldWidth WIDTH = FieldWidth::Scalar>
class EInterpEntity : public EntityView<Derived, WIDTH>
{
	TNX_REGISTER_SUPER_SCHEMA(EInterpEntity, EntityView, Transform, VisTransform)
	
	CTransform<WIDTH> Transform;
	CVisualTransform<WIDTH> VisTransform;
	
	void Initialize()
	{
		VisTransform.VisBlend = SimFloat(1.0f);
	}
	
	void PostPhysics([[maybe_unused]] SimFloat dt)
	{
		VisTransform.VisPosX = VisTransform.VisPosX + (Transform.PosX - VisTransform.VisPosX) * VisTransform.VisBlend;
		VisTransform.VisPosY = VisTransform.VisPosY + (Transform.PosY - VisTransform.VisPosY) * VisTransform.VisBlend;
		VisTransform.VisPosZ = VisTransform.VisPosZ + (Transform.PosZ - VisTransform.VisPosZ) * VisTransform.VisBlend;
	}
	
	void SetPosition(Vector3& NewPos)
	{
		VisTransform.VisPosX = NewPos.x;
		VisTransform.VisPosY = NewPos.y;
		VisTransform.VisPosZ = NewPos.z;
		Transform.PosX = NewPos.x;
		Transform.PosY = NewPos.y;
		Transform.PosZ = NewPos.z;
	}
};
