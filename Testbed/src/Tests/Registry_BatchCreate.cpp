#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Public/TestEntity.h"

// Validates batch entity creation: 100 handles returned, all non-zero, count increases by exactly 100.
// Uses a before/after delta rather than an absolute count so this test is robust to any entities
// left in the registry by preceding tests.
TEST(Registry_BatchCreate)
{
	Registry* Reg = Engine.GetRegistry();

	const uint32_t liveBase = Reg->GetTotalEntityCount();

	std::vector<EntityHandle> Handles = Reg->Create<TestEntity<>>(100);
	ASSERT_EQ(static_cast<int>(Handles.size()), 100);

	for (const EntityHandle& h : Handles)
		ASSERT_NE(h.GetHandleIndex(), 0u);

	ASSERT_EQ(Reg->GetTotalEntityCount(), liveBase + 100u);

	Engine.ResetRegistry();
}
