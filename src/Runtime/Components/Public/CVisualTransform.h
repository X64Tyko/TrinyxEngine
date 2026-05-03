#pragma once
#include "ComponentView.h"
#include "SchemaReflector.h"

// Visual position used for GPU rendering — intentionally decoupled from the authoritative
// CTransform position so that server corrections (which snap CTransform via rollback) can
// glide smoothly on screen instead of popping.
//
// Volatile (not rolled back): when a rollback snaps CTransform to the server-authoritative
// position, CVisualTransform stays put.  The entity's PostPhysics blend then converges it
// toward the new CTransform over the next N steps.
//
// GPU scatter reads VisPosX/Y/Z via SemPosX/Y/Z, not CTransform's PosX/Y/Z.
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct CVisualTransform : ComponentView<CVisualTransform, WIDTH>
{
	TNX_VOLATILE_FIELDS(CVisualTransform, Render, VisPosX, VisPosY, VisPosZ, VisBlend)

	FloatProxy<WIDTH> VisPosX;
	FloatProxy<WIDTH> VisPosY;
	FloatProxy<WIDTH> VisPosZ;
	FloatProxy<WIDTH> VisBlend;
	
	Vec3Accessor<WIDTH> Position{VisPosX, VisPosY, VisPosZ};
};
TNX_REGISTER_COMPONENT(CVisualTransform)
