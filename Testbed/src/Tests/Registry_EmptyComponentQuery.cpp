#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Archetype.h"
#include "Public/TestEntity.h"
#include "CJoltBody.h"

// Validates that ComponentQuery for a component NOT present on TestEntity does not include
// TestEntity's archetype. Regression guard: missing component must not produce false matches.
//
// NOTE: Registry_EmptyComponentQuery was originally written to assert Results.size() == 0
// when only a TestEntity exists. This fails because EInstanced (engine entity) is globally
// registered at startup with CJoltBody — ComponentQuery is never empty for CJoltBody.
// The test now asserts the correct property: TestEntity's archetype is excluded.
TEST(Registry_EmptyComponentQuery)
{
	Registry* Reg = Engine.GetRegistry();

	Reg->Create<TestEntity<>>();

	auto TestArches = Reg->ClassQuery<TestEntity<>>();
	ASSERT_EQ(static_cast<int>(TestArches.size()), 1);
	Archetype* TestArch = TestArches[0];

	// CJoltBody query must NOT include TestEntity's archetype
	auto Results = Reg->ComponentQuery<CJoltBody<>>();
	for (const Archetype* Arch : Results)
		ASSERT_NE(Arch, TestArch);

	Engine.ResetRegistry();
}
