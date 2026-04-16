#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "World.h"

// Validates that the ConstructRegistry is available by PostStart.
// ConstructRegistry is owned by FlowManager and passed to World — a null here
// means the OOP Construct layer has no backing store and all Construct ticks will
// silently drop.
RUNTIME_TEST(World_ConstructRegistryExists)
{
	ASSERT(Engine.GetDefaultWorld()->GetConstructRegistry() != nullptr);
}
