#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "World.h"

// Validates that the default world and its core subsystems are non-null by the time
// runtime tests execute. This is the baseline health check for all World-dependent tests.
RUNTIME_TEST(World_IsInitialized)
{
	World* W = Engine.GetDefaultWorld();
	ASSERT(W != nullptr);
	ASSERT(W->GetRegistry() != nullptr);
	ASSERT(W->GetPhysics() != nullptr);
	ASSERT(W->GetLogicThread() != nullptr);
	ASSERT(W->GetConstructRegistry() != nullptr);
}
