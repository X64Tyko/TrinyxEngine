#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Logger.h"
#include "Tests/TestbedHelpers.h"

#include <random>
#include <thread>
#include <SDL3/SDL_timer.h>

// Spawns a fan of projectiles from a single origin.
// Exercises: high-count minimal-field entities, velocity integration,
//            alpha fade, AVX2 wide-path throughput.
// Self-destructs after 30 seconds.
//
// Count is intentionally small so CI is fast. Increase to stress-test.
RUNTIME_TEST(Spawn_ProjectileBurst)
{
	std::mt19937 gen(std::random_device{}());
	std::uniform_real_distribution<float> spreadDist(-20.0f, 20.0f);
	std::uniform_real_distribution<float> speedDist(30.0f, 80.0f);
	std::uniform_real_distribution<float> colorDist(0.4f, 1.0f);

	constexpr int Count     = 100; // increase to stress-test
	constexpr SimFloat OriginY = SimFloat(20.0f);
	constexpr SimFloat OriginZ = SimFloat(-50.0f);

	static std::vector<ProjectileSetup> setups;
	setups.clear();
	setups.reserve(Count);

	for (int i = 0; i < Count; ++i)
	{
		SimFloat speed = speedDist(gen);
		setups.push_back({
			0.0f, OriginY, OriginZ,
			spreadDist(gen), spreadDist(gen) + SimFloat(10.0f), -speed,
			colorDist(gen), colorDist(gen) * SimFloat(0.5f), SimFloat(0.1f), SimFloat(1.0f)
		});
	}

	Registry* reg         = Engine.GetRegistry();
	const uint32_t before = reg->GetTotalEntityCount();

	Engine.Spawn([](uint32_t)
	{
		Registry* r = TrinyxEngine::Get().GetRegistry();
		WriteProjectileSetups(r, setups, gProjectileIds);
	});

	ASSERT_EQ(reg->GetTotalEntityCount() - before, static_cast<uint32_t>(Count));

	LOG_ENG_ALWAYS_F("[Spawn_ProjectileBurst] %d projectiles from origin (0, %.0f, %.0f) (30s lifetime)",
					 Count, OriginY.ToFloat(), OriginZ.ToFloat());

	std::thread([]()
	{
		SDL_Delay(30000);
		TrinyxEngine::Get().Spawn([](uint32_t)
		{
			Registry* r = TrinyxEngine::Get().GetRegistry();
			for (EntityHandle id : gProjectileIds) r->Destroy(id);
			LOG_ENG_ALWAYS_F("[Spawn_ProjectileBurst] Destroyed %zu entities after 30s", gProjectileIds.size());
			gProjectileIds.clear();
		});
	}).detach();
}
