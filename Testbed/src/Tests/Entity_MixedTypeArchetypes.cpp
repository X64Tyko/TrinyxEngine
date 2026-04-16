#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "World.h"
#include "Public/TestEntity.h"
#include "Public/CubeEntity.h"

// Validates that spawning two different entity types inside a live world creates two
// distinct archetypes that don't share the same pointer. If they aliased, ComponentQuery
// and ClassQuery would return wrong results.
RUNTIME_TEST(Entity_MixedTypeArchetypes)
{
	Engine.Spawn([](uint32_t)
	{
		Registry* Reg = TrinyxEngine::Get().GetRegistry();
		Reg->Create<TestEntity<>>();
		Reg->Create<CubeEntity<>>();
	});

	Registry* Reg = Engine.GetRegistry();
	auto TestArch = Reg->ClassQuery<TestEntity<>>();
	auto CubeArch = Reg->ClassQuery<CubeEntity<>>();

	ASSERT_EQ(static_cast<int>(TestArch.size()), 1);
	ASSERT_EQ(static_cast<int>(CubeArch.size()), 1);
	ASSERT_NE(TestArch[0], CubeArch[0]);
}
