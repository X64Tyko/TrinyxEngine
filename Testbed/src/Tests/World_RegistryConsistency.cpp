#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "World.h"

// Validates that Engine.GetRegistry() and Engine.GetDefaultWorld()->GetRegistry()
// return the same pointer. If these diverge it means the engine has two registries
// and all spawn/query calls that use the engine shortcut would operate on the wrong one.
RUNTIME_TEST(World_RegistryConsistency)
{
	Registry* EngineReg = Engine.GetRegistry();
	Registry* WorldReg  = Engine.GetDefaultWorld()->GetRegistry();

	ASSERT(EngineReg != nullptr);
	ASSERT(EngineReg == WorldReg);
}
