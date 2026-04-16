#pragma once
#include <FieldProxy.h>
#include "ComponentView.h"
#include "SchemaReflector.h"

// SpawnPoint tag component — marks a location as a valid player spawn.
// TeamID 0 = any team. Used by GameMode::ValidatePlayerBeginRequest.
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct CSpawnInfo : ComponentView<CSpawnInfo, WIDTH>
{
	TNX_VOLATILE_FIELDS(CSpawnInfo, Logic, TeamID)

	UIntProxy<WIDTH> TeamID; // 0 = any team
};

TNX_REGISTER_COMPONENT(CSpawnInfo)
