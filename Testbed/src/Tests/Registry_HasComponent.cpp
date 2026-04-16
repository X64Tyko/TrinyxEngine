#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Public/TestEntity.h"
#include "Public/CubeEntity.h"
#include "CJoltBody.h"
#include "CVelocity.h"
#include "CTransform.h"

// Validates that HasComponent<T> correctly reflects each entity type's component set.
// CubeEntity has CJoltBody; TestEntity does not.
TEST(Registry_HasComponent)
{
	Registry* Reg = Engine.GetRegistry();

	EntityHandle Cube = Reg->Create<CubeEntity<>>();
	EntityHandle Test = Reg->Create<TestEntity<>>();

	ASSERT(Reg->HasComponent<CJoltBody<>>(Cube));
	ASSERT(Reg->HasComponent<CVelocity<>>(Cube));
	ASSERT(Reg->HasComponent<CTransform<>>(Cube));

	ASSERT(Reg->HasComponent<CTransform<>>(Test));
	ASSERT(Reg->HasComponent<CVelocity<>>(Test));

	// TestEntity has no physics body
	ASSERT(!Reg->HasComponent<CJoltBody<>>(Test));

	Engine.ResetRegistry();
}
