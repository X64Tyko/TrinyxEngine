#pragma once
#include <FieldProxy.h>
#include "ComponentView.h"
#include "SchemaReflector.h"
#include "NetDelta.h"

// Control Rotation Component — the controlling Soul's view angles (radians), as simulation state.
//
// Written each fixed step from the frame's absolute view angles in sim input, so the Owner, the
// Authority, and every resimulated frame agree on facing by construction. Temporal: rolls back and
// replicates, so Echoes can aim and animate from it. Distinct from the body's CTransform rotation.
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct CControlRotation : ComponentView<CControlRotation, WIDTH>
{
	TNX_TEMPORAL_FIELDS(CControlRotation, None, Yaw, Pitch)

	FloatProxy<WIDTH> Yaw;
	FloatProxy<WIDTH> Pitch;
};

TNX_REGISTER_COMPONENT(CControlRotation)
TNX_NET_TEMPORAL(CControlRotation)
