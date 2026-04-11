#pragma once

#include "CSpawnInfo.h"
#include "CTransform.h"
#include "EntityView.h"
#include "SchemaReflector.h"

// Spawn point entity — a named world location where players can be spawned.
// GameMode::ValidateSpawnRequest iterates these to pick an authoritative spawn.
// Round-robin selection supported via TeamID=0 (any team).
template <FieldWidth WIDTH = FieldWidth::Scalar>
class ESpawnPoint : public EntityView<ESpawnPoint, WIDTH>
{
	TNX_REGISTER_SCHEMA(ESpawnPoint, EntityView, Transform, SpawnPoint)

public:
	CTransform<WIDTH> Transform;
	CSpawnInfo<WIDTH> SpawnPoint;

	FORCE_INLINE void PrePhysics([[maybe_unused]] SimFloat dt) {}
};
