#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "World.h"
#include "Public/TestEntity.h"
#include "Logger.h"

// Spawns 1000 TestEntities inside a live world and verifies the entity count increases
// by exactly 1000. Tests that batch Create<T>(N) works correctly inside the world queue
// path and that the Registry's live-count accounting doesn't drift.
RUNTIME_TEST(Spawn_LargeBatch)
{
	Registry* Reg = Engine.GetRegistry();
	uint32_t before = Reg->GetTotalEntityCount();

	Engine.Spawn([](uint32_t)
	{
		Registry* Reg = TrinyxEngine::Get().GetRegistry();
		Reg->Create<TestEntity<>>(1000);
	});

	uint32_t after = Reg->GetTotalEntityCount();
	LOG_ENG_ALWAYS_F("[Spawn_LargeBatch] %u → %u entities (delta %u)", before, after, after - before);

	ASSERT_EQ(static_cast<int>(after - before), 1000);
}
