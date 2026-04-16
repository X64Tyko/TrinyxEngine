#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Public/CubeEntity.h"
#include "CJoltBody.h"

// Validates that archetype signatures correctly reflect component membership.
// CubeEntity's archetype must have CJoltBody's bit set.
// SuperCube's archetype must NOT have CJoltBody's bit set.
TEST(Registry_ArchetypeSignature)
{
	Registry* Reg = Engine.GetRegistry();

	Reg->Create<CubeEntity<>>();
	Reg->Create<SuperCube<>>();

	auto CubeArches  = Reg->ClassQuery<CubeEntity<>>();
	auto SuperArches = Reg->ClassQuery<SuperCube<>>();

	ASSERT_EQ(static_cast<int>(CubeArches.size()), 1);
	ASSERT_EQ(static_cast<int>(SuperArches.size()), 1);

	// CJoltBody is 1-based; BuildSignature uses StaticTypeID()-1 for bit position.
	// Use ComponentQuery to test indirectly: JoltBody query should include CubeEntity but not SuperCube.
	auto JoltArches = Reg->ComponentQuery<CJoltBody<>>();
	bool cubeFound  = false, superFound = false;
	for (auto* arch : JoltArches)
	{
		if (arch == CubeArches[0]) cubeFound  = true;
		if (arch == SuperArches[0]) superFound = true;
	}

	ASSERT(cubeFound);
	ASSERT(!superFound);

	Engine.ResetRegistry();
}
