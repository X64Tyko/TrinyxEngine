#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Logger.h"
#include "Tests/TestbedHelpers.h"

#include <random>
#include <cmath>

// Spawns an N-tier pyramid of dynamic Jolt physics cubes on a static floor.
// Exercises: batch entity creation, Jolt body flush, collision response,
//            transform pull-back, GPU predicate/scatter with Active flags.
// Persistent — entities stay alive for the duration of the session.
//
// Non-rollback builds use the full height to exercise Jolt and the batch spawn
// path meaningfully. Rollback builds use a reduced height so the rollback
// determinism test re-simulates fewer entities and finishes in a reasonable time.
RUNTIME_TEST(Spawn_JoltPyramid)
{
	std::mt19937 gen(std::random_device{}());
	std::uniform_real_distribution<float> colorDist(0.2f, 1.0f);

	constexpr SimFloat cBoxSize       = SimFloat(2.0f);
	constexpr SimFloat cHalfBoxSize   = SimFloat(1.0f);
	constexpr SimFloat cBoxSeparation = SimFloat(0.5f);
#ifdef TNX_ENABLE_ROLLBACK
	constexpr int cPyramidHeight = 3; // reduced — rollback determinism test re-simulates these
#else
	constexpr int cPyramidHeight = 5;
#endif
	constexpr SimFloat xOffset = SimFloat(0.0f);
	constexpr SimFloat yOffset = SimFloat(-30.0f);
	constexpr SimFloat zOffset = SimFloat(-100.0f);

	static std::vector<CubeSetup> setups;
	setups.clear();

	for (int layer = 0; layer < cPyramidHeight; ++layer)
	{
		for (int j = layer / 2; j < cPyramidHeight - (layer + 1) / 2; ++j)
		{
			for (int k = layer / 2; k < cPyramidHeight - (layer + 1) / 2; ++k)
			{
				setups.push_back({
					xOffset + static_cast<SimFloat>(-cPyramidHeight) + cBoxSize * static_cast<SimFloat>(j) + ((layer & 1) ? cHalfBoxSize : 0.0f),
					yOffset + 1.0f + (cBoxSize + cBoxSeparation) * static_cast<SimFloat>(layer),
					zOffset + static_cast<SimFloat>(-cPyramidHeight) + cBoxSize * static_cast<SimFloat>(k) + ((layer & 1) ? cHalfBoxSize : 0.0f),
					cHalfBoxSize, cHalfBoxSize, cHalfBoxSize,
					1.0f,
					colorDist(gen), colorDist(gen), colorDist(gen),
					JoltMotion::Dynamic
				});
			}
		}
	}

	// Static floor
	setups.push_back({ xOffset, yOffset - 1.0f, zOffset, 50.0f, 1.0f, 50.0f, 0.0f, 0.3f, 0.3f, 0.3f, JoltMotion::Static });

	const uint32_t spawnCount = static_cast<uint32_t>(setups.size());

	Engine.Spawn([](uint32_t)
	{
		Registry* reg = TrinyxEngine::Get().GetRegistry();
		WriteCubeSetups(reg, setups, gPyramidIds);
	});

	ASSERT_EQ(static_cast<uint32_t>(gPyramidIds.size()), spawnCount);

	LOG_ENG_ALWAYS_F("[Spawn_JoltPyramid] %d-tier, %d dynamic + 1 floor = %zu entities (persistent)",
		cPyramidHeight, static_cast<int>(setups.size()) - 1, setups.size());
}
