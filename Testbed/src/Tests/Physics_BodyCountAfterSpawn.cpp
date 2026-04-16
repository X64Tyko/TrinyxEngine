#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "World.h"
#include "JoltPhysics.h"
#include "LogicThread.h"
#include "Registry.h"
#include "Tests/TestbedHelpers.h"
#include "Logger.h"
#include <thread>

// Validates that after spawning CubeEntities and waiting for FlushPendingBodies,
// the Jolt body count is non-zero.
//
// Sync strategy: Engine.Spawn() hands off the lambda to the Brain thread at the
// START of a tick (DrainWorldQueue), before FlushPendingBodies. DrainWorldQueue
// drains ALL queued jobs in one call, so multiple empty Engine.Spawn() calls can
// collapse into a single tick — they don't guarantee separate physics frames.
//
// Correct approach: record LastCompletedFrame before spawning, then spin until
// the Brain has completed at least PhysicsUpdateInterval+1 more frames.
// This guarantees FlushPendingBodies has run at least once since the spawn.
RUNTIME_TEST(Physics_BodyCountAfterSpawn)
{
	World* world       = Engine.GetDefaultWorld();
	LogicThread* logic = world->GetLogicThread();

	static std::vector<CubeSetup> setups;
	setups.clear();
	setups.push_back({ 0.0f, 10.0f, -50.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.5f, JoltMotion::Dynamic });
	setups.push_back({ 0.0f, -1.0f, -50.0f, 20.0f, 1.0f, 20.0f, 0.0f, 0.5f, 0.5f, 0.5f, JoltMotion::Static});

	// Record the frame before we spawn so we know when to stop waiting.
	uint32_t startFrame = logic->GetLastCompletedFrame();

	static std::vector<EntityHandle> ids;
	Engine.Spawn([](uint32_t)
	{
		Registry* reg = TrinyxEngine::Get().GetRegistry();
		WriteCubeSetups(reg, setups, ids);
	});

	// Wait until the Brain has completed enough frames to guarantee FlushPendingBodies ran.
	// PhysicsUpdateInterval is 8 by default; waiting for 9 completed frames after startFrame
	// ensures we've crossed at least one physics frame boundary.
	const uint32_t waitUntil = startFrame + 9;
	while (logic->GetLastCompletedFrame() < waitUntil) std::this_thread::yield();

	JoltPhysics* phys = world->GetPhysics();
	ASSERT(phys != nullptr);

	uint32_t numBodies = phys->GetBodyCount();
	LOG_ENG_ALWAYS_F("[Physics_BodyCountAfterSpawn] Jolt reports %u bodies", numBodies);

	ASSERT(numBodies > 0); // at least the 2 bodies from this test must be present
}
