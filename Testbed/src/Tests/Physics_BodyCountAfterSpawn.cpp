#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "World.h"
#include "JoltPhysics.h"
#include "Registry.h"
#include "Tests/TestbedHelpers.h"
#include "Logger.h"

// Validates that after spawning CubeEntities and waiting for FlushPendingBodies,
// the Jolt body count is non-zero.
//
// Sync strategy: Engine.Spawn() is a handshake — the lambda runs at the TOP of
// a Brain tick, BEFORE FlushPendingBodies. A second empty Engine.Spawn() waits for
// the NEXT Brain tick's handshake window, which only opens after the previous tick
// (and its FlushPendingBodies) fully completes. So after the second Spawn returns,
// all bodies from the first Spawn are guaranteed to be in Jolt.
//
// API hole note: JoltPhysics has no GetBodyCount() convenience wrapper.
// This test reaches into JPH::PhysicsSystem directly.
// TODO: add JoltPhysics::GetBodyCount() so gameplay code doesn't need the JPH header.
RUNTIME_TEST(Physics_BodyCountAfterSpawn)
{
	static std::vector<CubeSetup> setups;
	setups.clear();
	setups.push_back({ 0.0f, 10.0f, -50.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.5f, JoltMotion::Dynamic });
	setups.push_back({ 0.0f, -1.0f, -50.0f, 20.0f, 1.0f, 20.0f, 0.0f, 0.5f, 0.5f, 0.5f, JoltMotion::Static });

	static std::vector<EntityHandle> ids;
	Engine.Spawn([](uint32_t)
	{
		Registry* reg = TrinyxEngine::Get().GetRegistry();
		WriteCubeSetups(reg, setups, ids);
	});

	// Wait for the next handshake window — guarantees FlushPendingBodies ran for the tick above.
	Engine.Spawn([](uint32_t) {});

	JPH::PhysicsSystem* Sys = Engine.GetDefaultWorld()->GetPhysics()->GetPhysicsSystem();
	ASSERT(Sys != nullptr);

	uint32_t numBodies = Sys->GetNumBodies();
	LOG_ENG_ALWAYS_F("[Physics_BodyCountAfterSpawn] Jolt reports %u bodies", numBodies);

	ASSERT(numBodies > 0); // at least the 2 bodies from this test must be present
}
