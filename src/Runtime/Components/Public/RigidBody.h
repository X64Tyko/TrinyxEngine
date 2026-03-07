#pragma once
#include "ComponentView.h"
#include "SchemaReflector.h"

// RigidBody component — linear and angular velocity for physics simulation.
// Temporal: stored in N-frame rollback ring buffer. Required for deterministic
// resimulation during rollback netcode. Physics partition group (Phys).
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct RigidBody : ComponentView<RigidBody, WIDTH>
{
	TNX_TEMPORAL_FIELDS(RigidBody, Physics, VelX, VelY, VelZ, AngVelX, AngVelY, AngVelZ)

	FloatProxy<WIDTH> VelX;
	FloatProxy<WIDTH> VelY;
	FloatProxy<WIDTH> VelZ;

	FloatProxy<WIDTH> AngVelX;
	FloatProxy<WIDTH> AngVelY;
	FloatProxy<WIDTH> AngVelZ;
};

TNX_REGISTER_COMPONENT(RigidBody)