#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Public/TestEntity.h"

// Validates that a destroyed slot is reused by the next allocation
// (generation bump + slot recycling).
TEST(Registry_DestroyAndReuse)
{
	Registry* Reg = Engine.GetRegistry();

	std::vector<EntityHandle> Entities;
	for (int i = 0; i < 10; ++i)
		Entities.push_back(Reg->Create<TestEntity<>>());

	uint32_t firstHandleIndex = Entities[0].GetHandleIndex();

	Reg->Destroy(Entities[0]);
	Reg->ProcessDeferredDestructions();
	Engine.ConfirmLocalRecycles();

	EntityHandle NewId = Reg->Create<TestEntity<>>();
	ASSERT_EQ(NewId.GetHandleIndex(), firstHandleIndex);

	Engine.ResetRegistry();
}
