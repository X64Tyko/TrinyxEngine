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
// Count is set to 0 by default — change to stress-test the projectile path.
RUNTIME_TEST(Spawn_ProjectileBurst)
{
	std::mt19937 gen(std::random_device{}());
	std::uniform_real_distribution<float> spreadDist(-20.0f, 20.0f);
	std::uniform_real_distribution<float> speedDist(30.0f, 80.0f);
	std::uniform_real_distribution<float> colorDist(0.4f, 1.0f);

	constexpr int   Count    = 0; // increase to stress-test
	constexpr float OriginY  = 20.0f;
	constexpr float OriginZ  = -50.0f;

	static std::vector<ProjectileSetup> setups;
	setups.clear();
	setups.reserve(Count);

	for (int i = 0; i < Count; ++i)
	{
		float speed = speedDist(gen);
		setups.push_back({
			0.0f, OriginY, OriginZ,
			spreadDist(gen), spreadDist(gen) + 10.0f, -speed,
			colorDist(gen), colorDist(gen) * 0.5f, 0.1f, 1.0f
		});
	}

	Engine.Spawn([](uint32_t)
	{
		Registry* reg = TrinyxEngine::Get().GetRegistry();
		WriteProjectileSetups(reg, setups, gProjectileIds);
	});

	LOG_ENG_ALWAYS_F("[Spawn_ProjectileBurst] %d projectiles from origin (0, %.0f, %.0f) (30s lifetime)",
		Count, OriginY, OriginZ);

	if (Count > 0)
	{
		std::thread([]()
		{
			SDL_Delay(30000);
			TrinyxEngine::Get().Spawn([](uint32_t)
			{
				Registry* reg = TrinyxEngine::Get().GetRegistry();
				for (EntityHandle id : gProjectileIds) reg->Destroy(id);
				LOG_ENG_ALWAYS_F("[Spawn_ProjectileBurst] Destroyed %zu entities after 30s", gProjectileIds.size());
				gProjectileIds.clear();
			});
		}).detach();
	}
}
