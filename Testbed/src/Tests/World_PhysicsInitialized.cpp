#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "World.h"
#include "JoltPhysics.h"

// Validates that Jolt's physics system and temp allocator are valid by PostStart.
// Confirms the physics subsystem is ready for body creation and stepping.
RUNTIME_TEST(World_PhysicsInitialized)
{
	JoltPhysics* Phys = Engine.GetDefaultWorld()->GetPhysics();
	ASSERT(Phys != nullptr);
	ASSERT(Phys->GetPhysicsSystem() != nullptr);
	ASSERT(Phys->GetTempAllocator() != nullptr);
}
