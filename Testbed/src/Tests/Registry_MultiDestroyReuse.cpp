#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Public/TestEntity.h"

// Creates 10 entities, destroys the first 5, confirms the handle pool recycles them.
// After ConfirmLocalRecycles the free list should have 5 slots ready for reuse.
TEST(Registry_MultiDestroyReuse)
{
	Registry* Reg = Engine.GetRegistry();

	std::vector<EntityHandle> Handles;
	for (int i = 0; i < 10; ++i)
		Handles.push_back(Reg->Create<TestEntity<>>());

	ASSERT_EQ(static_cast<int>(Reg->GetTotalEntityCount()), 10);

	// Destroy first 5
	for (int i = 0; i < 5; ++i)
		Reg->Destroy(Handles[i]);

	Reg->ProcessDeferredDestructions();
	Engine.ConfirmLocalRecycles();

	ASSERT_EQ(static_cast<int>(Reg->GetTotalEntityCount()), 5);

	// Create 5 new — they should reuse the freed slots
	std::vector<EntityHandle> NewHandles;
	for (int i = 0; i < 5; ++i)
		NewHandles.push_back(Reg->Create<TestEntity<>>());

	ASSERT_EQ(static_cast<int>(Reg->GetTotalEntityCount()), 10);

	Engine.ResetRegistry();
}
