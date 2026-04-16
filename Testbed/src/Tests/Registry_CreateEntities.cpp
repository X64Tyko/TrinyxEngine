#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Public/TestEntity.h"

// Validates basic entity batch creation and handle validity.
TEST(Registry_CreateEntities)
{
	Registry* Reg = Engine.GetRegistry();

	std::vector<EntityHandle> Entities;
	for (int i = 0; i < 5; ++i)
		Entities.push_back(Reg->Create<TestEntity<>>());

	ASSERT_EQ(static_cast<int>(Entities.size()), 5);

	Engine.ResetRegistry();
}
