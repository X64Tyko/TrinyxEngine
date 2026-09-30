#include "TrinyxEngine.h"
#include "GameManager.h"
#include "TestFramework.h"
#include "World.h"
#include "Input.h"
#include "PlayerConstruct.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef TNX_ENABLE_NETWORK
#include "GameMode.h"
#include "ReplicationSystem.h"
#include "ReflectionRegistry.h"
#include "SchemaReflector.h"
#endif

// Pull in all test files (GLOB_RECURSE in CMakeLists already compiles them;
// this include list is intentionally NOT here — each Tests/*.cpp is its own
// translation unit compiled by CMake's file(GLOB_RECURSE TESTBED_SOURCES)).

using namespace tnx::Testing;

#ifdef TNX_ENABLE_NETWORK
// ---------------------------------------------------------------------------
// TestNetGameMode — server-side player spawn for PIE / networked tests.
// ---------------------------------------------------------------------------
class TestNetGameMode : public GameMode
{
public:
	PlayerBeginResult OnPlayerBeginRequest(Soul& soul, const PlayerBeginRequestPayload& /*req*/) override
	{
		PlayerBeginResult result;
		result.Accepted = true;

		static constexpr SimFloat SpawnPoints[2][3] = {
			{ SimFloat(2.0f), SimFloat(5.0f), SimFloat(0.0f) },
			{ SimFloat(-2.0f), SimFloat(5.0f), SimFloat(0.0f) },
		};
		const uint8_t idx = SpawnCounter.fetch_add(1, std::memory_order_relaxed) % 2;
		result.PosX       = SpawnPoints[idx][0];
		result.PosY       = SpawnPoints[idx][1];
		result.PosZ       = SpawnPoints[idx][2];

		WorldBase* world        = GetWorld();
		ReplicationSystem* repl = world->GetReplicationSystem();
		const uint16_t typeHash = ReflectionRegistry::ConstructTypeHashFromName("PlayerConstruct");

		// Spawn lambda must be trivially copyable (ValidJobLambda contract) and
		// synchronous (SpawnAndWait) so bodyRef is populated before ClaimBody.
		// Capture raw pointers + values instead of [&] to satisfy the constraint.
		ConstructRef bodyRef{};
		ConstructRef* bodyRefPtr = &bodyRef;
		Soul* soulPtr            = &soul;
		SimFloat posX = result.PosX, posY = result.PosY, posZ = result.PosZ;

		world->SpawnAndWait([world, repl, soulPtr, typeHash, posX, posY, posZ, bodyRefPtr](uint32_t)
		{
			ConstructRegistry* reg  = world->GetConstructRegistry();
			PlayerConstruct* player = reg->Create<PlayerConstruct>(world, [soulPtr, posX, posY, posZ](PlayerConstruct* p)
			{
				p->SpawnPosX = posX;
				p->SpawnPosY = posY;
				p->SpawnPosZ = posZ;
				p->SetOwnerSoul(soulPtr);
			});
			*bodyRefPtr             = repl->RegisterConstruct(reg, player, soulPtr->GetOwnerID(), typeHash, 0);
		});

		soul.ClaimBody(bodyRef);
		result.Body = bodyRef;
		return result;
	}

private:
	std::atomic<uint8_t> SpawnCounter{ 0 };
};

TNX_REGISTER_MODE(TestNetGameMode)
#endif // TNX_ENABLE_NETWORK

// ---------------------------------------------------------------------------
// TestbedGame — drives the test suite.
//
// CLI:
//   --test <Name>    Run only the named test. Repeatable to run a subset.
//   --list-tests     Print all registered test names and exit.
//
// If no --test args are given, all registered tests run.
// ---------------------------------------------------------------------------
class TestbedGame : public GameManager<TestbedGame>
{
public:
	const char* GetWindowTitle() const { return "Trinyx Testbed"; }

	// Parse Testbed-specific args before engine initialization.
	void PreInitialize(int argc, char* argv[])
	{
		for (int i = 1; i < argc; ++i)
		{
			if (strcmp(argv[i], "--test") == 0 && i + 1 < argc)
				SelectedTests.push_back(argv[++i]);
			else if (strcmp(argv[i], "--list-tests") == 0)
				ListTestsAndExit = true;
		}
	}

	bool PostInitialize(TrinyxEngine& engine)
	{
		if (ListTestsAndExit)
		{
			tnx::Testing::ListAllTests();
			return false; // abort before the loop
		}

		if (!SelectedTests.empty())
		{
			std::cout << "\nRunning " << SelectedTests.size() << " selected test(s):\n";
			for (const auto& n : SelectedTests)
				std::cout << "  " << n << "\n";
		}

		const int failed = TestRegistry::Instance().RunFiltered(engine, SelectedTests);
		return failed == 0;
	}

	void PostStart(TrinyxEngine& engine)
	{
		// Watchdog: abort if the Logic thread stops advancing frames.
		// GetLastCompletedFrame() ticks at 512 Hz — any genuine progress resets the clock.
		// The threshold only needs to cover the longest single-tick operation (e.g. a full
		// rollback resim in a debug build).
#ifdef NDEBUG
		constexpr auto kStuckThreshold = std::chrono::seconds(30);
#else
		constexpr auto kStuckThreshold = std::chrono::seconds(60);
#endif
		static std::atomic<bool> suiteDone{ false };
		std::thread watchdog([&]()
		{
			uint32_t lastFrame = 0;
			auto lastTick      = std::chrono::steady_clock::now();

			while (!suiteDone.load(std::memory_order_acquire))
			{
				std::this_thread::sleep_for(std::chrono::seconds(1));
				if (suiteDone.load(std::memory_order_acquire)) break;

				WorldBase* world       = engine.GetDefaultWorld();
				LogicThreadBase* logic = world ? world->GetLogicThread() : nullptr;
				if (!logic) continue;

				const uint32_t frame = logic->GetLastCompletedFrame();
				if (frame != lastFrame)
				{
					lastFrame = frame;
					lastTick  = std::chrono::steady_clock::now();
				}
				else if (std::chrono::steady_clock::now() - lastTick >= kStuckThreshold)
				{
					std::cout << "\n[Testbed] Logic thread stuck at frame " << lastFrame << " — aborting.\n";
					std::cout.flush();
					std::exit(1);
				}
			}
		});

		RuntimeFailures = RuntimeTestRegistry::Instance().RunFiltered(engine, SelectedTests);

		suiteDone.store(true, std::memory_order_release);
		watchdog.join();
	}

	int GetExitCode() const { return RuntimeFailures > 0 ? 1 : 0; }

private:
	std::vector<std::string> SelectedTests;
	bool ListTestsAndExit = false;
	int RuntimeFailures   = 0;
};

TNX_IMPLEMENT_GAME(TestbedGame)
