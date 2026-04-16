#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Public/TestEntity.h"
#include "Public/CubeEntity.h"

// Validates ClassQuery<T...>:
//   - Each entity type has exactly one archetype
//   - Two different entity types return different archetype pointers
TEST(Registry_ClassQuery)
{
	Registry* Reg = Engine.GetRegistry();

	Reg->Create<TestEntity<>>();
	Reg->Create<CubeEntity<>>();

	auto TestArches = Reg->ClassQuery<TestEntity<>>();
	ASSERT_EQ(static_cast<int>(TestArches.size()), 1);

	auto CubeArches = Reg->ClassQuery<CubeEntity<>>();
	ASSERT_EQ(static_cast<int>(CubeArches.size()), 1);

	// They are different archetypes
	ASSERT_NE(TestArches[0], CubeArches[0]);

	Engine.ResetRegistry();
}
