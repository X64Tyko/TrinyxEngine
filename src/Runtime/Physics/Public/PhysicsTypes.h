#pragma once

#include "EntityRecord.h"
#include "Types.h"

// HitConstruct / OverlappedConstruct: non-null when the other body is Construct-owned.
// Raw void* — valid for the duration of the callback only. Do not store.

struct PhysicsOnHitData
{
	EntityHandle HitEntity;
	void* HitConstruct = nullptr;
	Vector3 HitNormal;
	float Penetration;
};

struct PhysicsOverlapData
{
	EntityHandle OverlappedEntity;
	void* OverlappedConstruct = nullptr;
};
