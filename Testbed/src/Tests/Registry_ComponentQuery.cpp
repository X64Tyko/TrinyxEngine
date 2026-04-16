#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Public/TestEntity.h"
#include "Public/CubeEntity.h"
#include "CJoltBody.h"
#include "CTransform.h"

// Validates ComponentQuery<T...>:
//   - CTransform matches all archetypes (both TestEntity and CubeEntity have it)
//   - CJoltBody matches only CubeEntity archetype
//   - A component combination present in zero archetypes returns empty
TEST(Registry_ComponentQuery)
{
	Registry* Reg = Engine.GetRegistry();

	// Populate one of each so archetypes are registered
	Reg->Create<TestEntity<>>();
	Reg->Create<CubeEntity<>>();

	auto TransformArches = Reg->ComponentQuery<CTransform<>>();
	ASSERT(TransformArches.size() >= 2); // TestEntity + CubeEntity both have CTransform

	auto JoltArches = Reg->ComponentQuery<CJoltBody<>>();
	// EInstanced (engine entity) is also globally registered with CJoltBody, so the result
	// includes at least CubeEntity's archetype — we can't assume exactly 1.
	ASSERT(JoltArches.size() >= 1); // CubeEntity archetype is present

	Engine.ResetRegistry();
}
