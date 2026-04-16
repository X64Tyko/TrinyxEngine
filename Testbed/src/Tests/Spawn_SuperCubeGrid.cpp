#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Logger.h"
#include "Tests/TestbedHelpers.h"

#include <random>
#include <cmath>
#include <thread>
#include <SDL3/SDL_timer.h>

// Spawns a large grid of SuperCube (non-physics, color-animating) entities.
// Exercises: high entity count, ScalarUpdate color animation, no Jolt overhead.
// Self-destructs after 30 seconds.
RUNTIME_TEST(Spawn_SuperCubeGrid)
{
	std::mt19937 gen(std::random_device{}());
	std::uniform_real_distribution<float> colorDist(0.2f, 1.0f);

	constexpr int   Count    = 10000;
	constexpr float Spacing  = 3.0f;
	constexpr float CubeHalf = 0.5f;
	constexpr float YBase    = 10.0f;
	constexpr float ZOffset  = -200.0f;

	int   gridSide = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(Count))));
	float gridHalf = static_cast<float>(gridSide) * Spacing * 0.5f;

	static std::vector<CubeSetup> setups;
	setups.clear();
	setups.reserve(Count);

	for (int i = 0; i < Count; ++i)
	{
		int row = i / gridSide;
		int col = i % gridSide;
		setups.push_back({
			static_cast<float>(col) * Spacing - gridHalf,
			YBase + static_cast<float>(row) * Spacing,
			ZOffset,
			CubeHalf, CubeHalf, CubeHalf,
			0.0f,
			colorDist(gen), colorDist(gen), colorDist(gen),
			JoltMotion::Static
		});
	}

	Engine.Spawn([](uint32_t)
	{
		Registry* reg = TrinyxEngine::Get().GetRegistry();
		WriteSuperCubeSetups(reg, setups, gSuperCubeIds);
	});

	LOG_ENG_ALWAYS_F("[Spawn_SuperCubeGrid] %d entities in %dx%d grid (30s lifetime)",
		Count, gridSide, gridSide);

	// Self-destruct after 30 seconds
	std::thread([]()
	{
		SDL_Delay(30000);
		TrinyxEngine::Get().Spawn([](uint32_t)
		{
			Registry* reg = TrinyxEngine::Get().GetRegistry();
			for (EntityHandle id : gSuperCubeIds) reg->Destroy(id);
			LOG_ENG_ALWAYS_F("[Spawn_SuperCubeGrid] Destroyed %zu entities after 30s", gSuperCubeIds.size());
			gSuperCubeIds.clear();
		});
	}).detach();
}
