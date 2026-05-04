#pragma once

#include "EntityView.h"
#include "SchemaReflector.h"
#include "CTransform.h"
#include "CCameraLayer.h"

// ECamera — scripted runtime camera driven by a Construct or animation system.
template <FieldWidth WIDTH = FieldWidth::Scalar>
class ECamera : public EntityView<ECamera, WIDTH>
{
	TNX_REGISTER_SCHEMA(ECamera, EntityView, Transform, CameraLayer)

public:
	CTransform<WIDTH>  Transform;
	CCameraLayer<WIDTH> CameraLayer;

};
