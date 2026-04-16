#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Logger.h"

// Validates that at least one entity is alive after the spawn tests have run.
// Run this after Spawn_JoltPyramid, Spawn_SuperCubeGrid, or any other spawn test.
// As a standalone --test this will fail (no entities spawned yet).
// Example usage: --test Spawn_JoltPyramid --test Spawn_Validate_EntityCountValid
RUNTIME_TEST(Spawn_Validate_EntityCountValid)
{
	Registry* Reg        = Engine.GetRegistry();
	size_t totalEntities = Reg->GetTotalEntityCount();
	LOG_ENG_ALWAYS_F("[Spawn_Validate_EntityCountValid] Total entities alive: %zu", totalEntities);
	ASSERT(totalEntities > 0);
}
