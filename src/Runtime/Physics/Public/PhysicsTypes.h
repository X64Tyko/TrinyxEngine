#pragma once

#include "EntityRecord.h"
#include "Types.h"

// ---------------------------------------------------------------------------
// Gameplay-facing physics event data. No Jolt dependency.
// Constructs receive these via concept-detected OnHit / OnOverlapBegin / OnOverlapEnd.
// ---------------------------------------------------------------------------

struct PhysicsOnHitData
{
	EntityHandle HitEntity;
	Vector3 HitNormal;
	float Penetration;
};

struct PhysicsOverlapData
{
	EntityHandle OverlappedEntity;
};
