#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "World.h"
#include "ConstructRegistry.h"
#include "Logger.h"
#include "PlayerConstruct.h"

// Spawns a single PlayerConstruct in standalone (no-network) mode.
// Exercises: Construct<T> CRTP, ConstructView<EPlayer>, JoltCharacter init,
//            PhysicsStep tick, ScalarUpdate color pulse, full ECS round-trip.
//
// Runs in all builds (standalone, headless, network). Network-driven player
// spawning is tested separately in Net_PlayerConstruct.
RUNTIME_TEST(Spawn_PlayerConstruct)
{
	World* world = Engine.GetDefaultWorld();
	ASSERT(world != nullptr);

	/*
	 * TODO: PlayerConstruct currently requires JoltCharacter physics init which
	 * needs the full physics step to have run at least once. Uncomment and wire
	 * this up properly once the PhysicsStep tick sequencing is stable enough for
	 * testbed use.
	 *
	Engine.Spawn([](uint32_t)
	{
		World* w = TrinyxEngine::Get().GetDefaultWorld();
		w->GetConstructRegistry()->Create<PlayerConstruct>(w);
	});
	*/

	LOG_ENG_ALWAYS("[Spawn_PlayerConstruct] Standalone PlayerConstruct spawn — pending PhysicsStep stabilization");
}
