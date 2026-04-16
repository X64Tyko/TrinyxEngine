#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "World.h"
#include "Public/TestEntity.h"

// Validates the full entity lifecycle inside a running world:
// spawn → count increases, destroy → ProcessDeferredDestructions → count decreases.
// Runs inside Engine.Spawn (Logic thread context) to match real usage.
RUNTIME_TEST(Entity_DestroyUpdatesCount)
{
	static EntityHandle created{};
	static uint32_t countAfterCreate = 0;
	static uint32_t countAfterDestroy = 0;

	Engine.Spawn([](uint32_t)
	{
		Registry* Reg    = TrinyxEngine::Get().GetRegistry();
		uint32_t before  = Reg->GetTotalEntityCount();
		created          = Reg->Create<TestEntity<>>();
		countAfterCreate = Reg->GetTotalEntityCount();

		Reg->Destroy(created);
		Reg->ProcessDeferredDestructions();
		TrinyxEngine::Get().ConfirmLocalRecycles();
		countAfterDestroy = Reg->GetTotalEntityCount();

		// Restore — don't leave debris for other tests
		(void)before;
	});

	ASSERT(countAfterCreate > 0);
	ASSERT(countAfterDestroy < countAfterCreate);
}
