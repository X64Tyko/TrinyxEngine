#include "TrinyxEngine.h"
#include "GameManager.h"
#include "TestFramework.h"
#include "World.h"
#include "Input.h"
#include "PlayerConstruct.h"

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

		static constexpr float SpawnPoints[2][3] = {
			{  2.0f, 5.0f, 0.0f },
			{ -2.0f, 5.0f, 0.0f },
		};
		const uint8_t idx  = SpawnCounter.fetch_add(1, std::memory_order_relaxed) % 2;
		result.PosX = SpawnPoints[idx][0];
		result.PosY = SpawnPoints[idx][1];
		result.PosZ = SpawnPoints[idx][2];

		WorldBase* world    = GetWorld();
		ReplicationSystem* repl     = world->GetReplicationSystem();
		const uint16_t     typeHash = ReflectionRegistry::ConstructTypeHashFromName("PlayerConstruct");

		// Spawn lambda must be trivially copyable (ValidJobLambda contract) and
		// synchronous (SpawnAndWait) so bodyRef is populated before ClaimBody.
		// Capture raw pointers + values instead of [&] to satisfy the constraint.
		ConstructRef bodyRef{};
		ConstructRef* bodyRefPtr = &bodyRef;
		Soul* soulPtr            = &soul;
		float posX               = result.PosX, posY = result.PosY, posZ = result.PosZ;

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
			*bodyRefPtr = repl->RegisterConstruct(reg, player, soulPtr->GetOwnerID(), typeHash, 0);
		});

		soul.ClaimBody(bodyRef);
		result.Body = bodyRef;
		return result;
	}

private:
	std::atomic<uint8_t> SpawnCounter{0};
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
			for (const auto& n : SelectedTests) std::cout << "  " << n << "\n";
		}

		const int failed = TestRegistry::Instance().RunFiltered(engine, SelectedTests);
		return failed == 0;
	}

	void PostStart(TrinyxEngine& engine)
	{
		RuntimeFailures = RuntimeTestRegistry::Instance().RunFiltered(engine, SelectedTests);
	}

	int GetExitCode() const { return RuntimeFailures > 0 ? 1 : 0; }

private:
	std::vector<std::string> SelectedTests;
	bool ListTestsAndExit = false;
	int RuntimeFailures   = 0;
};

TNX_IMPLEMENT_GAME(TestbedGame)

