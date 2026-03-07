#pragma once
#include "ComponentView.h"
#include "SchemaReflector.h"

// Forces component — accumulated linear force and torque for the current simulation step.
// Zeroed at the start of each PrePhysics pass by the physics system; values written
// during PrePhysics are consumed by the solver during the physics step.
// Volatile: no rollback needed — forces are re-derived from inputs during resimulation.
// Physics partition group (Phys).
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct Forces : ComponentView<Forces, WIDTH>
{
	TNX_VOLATILE_FIELDS(Forces, Physics, ForceX, ForceY, ForceZ, TorqueX, TorqueY, TorqueZ)

	FloatProxy<WIDTH> ForceX;
	FloatProxy<WIDTH> ForceY;
	FloatProxy<WIDTH> ForceZ;

	FloatProxy<WIDTH> TorqueX;
	FloatProxy<WIDTH> TorqueY;
	FloatProxy<WIDTH> TorqueZ;
};

TNX_REGISTER_COMPONENT(Forces)