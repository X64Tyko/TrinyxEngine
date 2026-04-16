#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Public/TestEntity.h"

// Validates that all created handles report IsValid().
TEST(Registry_ValidEntityIDs)
{
	Registry* Reg = Engine.GetRegistry();

	std::vector<EntityHandle> Entities;
	for (int i = 0; i < 100; ++i)
		Entities.push_back(Reg->Create<TestEntity<>>());

	for (EntityHandle Id : Entities)
		ASSERT(Id.IsValid());

	Engine.ResetRegistry();
}
