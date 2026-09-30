#if defined(TNX_ENABLE_ROLLBACK) && defined(TNX_TESTING)

#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Logger.h"
#include "Tests/TestbedHelpers.h"
#include "WorldBase.h"
#include "LogicThreadBase.h"

#include <SDL3/SDL_timer.h>

// Validates rollback determinism: saves ECS + Jolt state, rolls back RollbackSim::RollbackFrameCount
// frames, resimulates forward with the same recorded inputs, then byte-compares the result.
//
// Relies on Spawn_JoltPyramid having run first (gPyramidIds populated) to give the resimulation
// meaningful physics work. Falls back to spawning a minimal floor + dynamic cube if needed.
//
// Requires: TNX_ENABLE_ROLLBACK=ON, TNX_TESTING defined (Testbed build)
RUNTIME_TEST(Rollback_Determinism)
{
	LogicThreadBase* logic = Engine.GetDefaultWorld()->GetLogicThread();
	ASSERT(logic != nullptr);

	Registry* reg = Engine.GetRegistry();
	ASSERT(reg != nullptr);
	ASSERT(reg->GetTemporalCache() != nullptr);

	// Ensure there are physics entities in the world so the resimulation exercises Jolt.
	// Spawn_JoltPyramid (persistent) populates gPyramidIds if it ran earlier in the suite.
	static std::vector<EntityHandle> localIds;
	if (gPyramidIds.empty())
	{
		static std::vector<CubeSetup> fallbackSetups;
		fallbackSetups.clear();
		fallbackSetups.push_back({ SimFloat(0), SimFloat(5), SimFloat(-50),
			SimFloat(1), SimFloat(1), SimFloat(1),
			SimFloat(1), SimFloat(0.8f), SimFloat(0.4f), SimFloat(0.2f),
			JoltMotion::Dynamic });
		fallbackSetups.push_back({ SimFloat(0), SimFloat(0), SimFloat(-50),
			SimFloat(25), SimFloat(1), SimFloat(25),
			SimFloat(0), SimFloat(0.3f), SimFloat(0.3f), SimFloat(0.3f),
			JoltMotion::Static });

		Engine.Spawn([](uint32_t)
		{
			Registry* r = TrinyxEngine::Get().GetRegistry();
			WriteCubeSetups(r, fallbackSetups, localIds);
		});
		ASSERT(!localIds.empty());
	}

	// Wait for the ring buffer to accumulate enough history.
	// ExecuteRollbackTest needs FrameNumber > RollbackSim::RollbackFrameCount + PhysicsDivizor.
	// 32 frames is comfortably above the threshold for any supported physics divisor.
#ifdef NDEBUG
	constexpr uint64_t kWarmupMs = 5000;
	constexpr uint64_t kTestMs   = 30000;
#else
	constexpr uint64_t kWarmupMs = 15000;
	constexpr uint64_t kTestMs   = 60000;
#endif

	const uint32_t kMinFrames     = logic->GetLastCompletedFrame() + 32;
	const uint64_t warmupDeadline = SDL_GetTicks() + kWarmupMs;
	while (logic->GetLastCompletedFrame() < kMinFrames && SDL_GetTicks() < warmupDeadline)
		SDL_Delay(5);
	ASSERT(logic->GetLastCompletedFrame() >= kMinFrames);

	// Arm the self-test — fires on the next ProcessRollback call inside the Logic thread.
	logic->RequestRollbackTest();

	// Block until ExecuteRollbackTest sets bRollbackTestComplete.
	const uint64_t testDeadline = SDL_GetTicks() + kTestMs;
	while (!logic->IsRollbackTestComplete() && SDL_GetTicks() < testDeadline)
		SDL_Delay(5);
	ASSERT(logic->IsRollbackTestComplete());

	ASSERT(logic->GetRollbackTestPassed());

	LOG_ENG_ALWAYS("[Rollback_Determinism] PASSED — resimulation is byte-identical");
}

#endif // TNX_ENABLE_ROLLBACK && TNX_TESTING
