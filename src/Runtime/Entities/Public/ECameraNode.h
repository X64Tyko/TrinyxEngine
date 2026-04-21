#pragma once

#include "EntityView.h"
#include "SchemaReflector.h"
#include "CNodeTransform.h"
#include "CCameraLayer.h"

// ECameraNode — editor-placed lobby/loading camera. Cold storage, Logic partition.
template <FieldWidth WIDTH = FieldWidth::Scalar>
class ECameraNode : public EntityView<ECameraNode, WIDTH>
{
	TNX_REGISTER_SCHEMA(ECameraNode, EntityView, NodeTransform, CameraLayer)

public:
	CNodeTransform<WIDTH> NodeTransform;
	CCameraLayer<WIDTH>   CameraLayer;

	FORCE_INLINE void PrePhysics([[maybe_unused]] SimFloat dt) {}
};
