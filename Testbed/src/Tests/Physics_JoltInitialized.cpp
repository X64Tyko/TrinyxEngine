#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "World.h"
#include "JoltPhysics.h"

// Validates that Jolt's internals are live by the time runtime tests run.
// Specifically: GetPhysicsSystem() and GetTempAllocator() must both be valid.
// A null TempAllocator would crash JoltCharacter::Update silently.
RUNTIME_TEST(Physics_JoltInitialized)
{
	JoltPhysics* Phys = Engine.GetDefaultWorld()->GetPhysics();
	ASSERT(Phys != nullptr);

	JPH::PhysicsSystem* Sys = Phys->GetPhysicsSystem();
	ASSERT(Sys != nullptr);

	JPH::TempAllocator* Alloc = Phys->GetTempAllocator();
	ASSERT(Alloc != nullptr);
}
