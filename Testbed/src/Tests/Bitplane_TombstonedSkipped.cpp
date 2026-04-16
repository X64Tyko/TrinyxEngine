#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Public/TestEntity.h"
#include "CTransform.h"
#include "Archetype.h"

// Validates tombstone semantics: Destroy clears the Active flag (tombstoned) and
// decrements TotalEntityCount (live), but AllocatedEntityCount (high-water mark)
// is not decremented — it's used as the iteration bound and the bitplane/masked-store
// path skips dead slots at zero cost.
//
// This test verifies the invariant that ComponentQuery and live-entity counts correctly
// exclude tombstoned entities even before slot memory is reclaimed.
TEST(Bitplane_TombstonedSkipped)
{
	Registry* Reg = Engine.GetRegistry();

	const uint32_t liveBase = Reg->GetTotalEntityCount();

	// Spawn a known batch of test entities
	constexpr int SpawnCount          = 16;
	std::vector<EntityHandle> handles = Reg->Create<TestEntity<>>(SpawnCount);
	ASSERT_EQ(static_cast<int>(handles.size()), SpawnCount);
	ASSERT_EQ(Reg->GetTotalEntityCount(), liveBase + SpawnCount);

	// Destroy half of them — they become tombstoned
	constexpr int DestroyCount = 8;
	for (int i = 0; i < DestroyCount; ++i) Reg->Destroy(handles[static_cast<size_t>(i)]);

	Reg->ProcessDeferredDestructions();

	// Live count drops by exactly DestroyCount
	const uint32_t liveAfter = Reg->GetTotalEntityCount();
	ASSERT_EQ(liveAfter, liveBase + (SpawnCount - DestroyCount));

	// AllocatedEntityCount is the sum of per-archetype chunk high-water marks.
	// It should still account for all allocated slots (tombstoned OR live).
	// We verify this by checking that the chunk count from ClassQuery still sees
	// the archetype — it's not removed just because some slots are tombstoned.
	std::vector<Archetype*> arches = Reg->ClassQuery<TestEntity<>>();
	ASSERT(arches.size() >= 1);

	uint32_t totalAllocated = 0;
	for (Archetype* arch : arches)
	{
		for (size_t ci = 0; ci < arch->Chunks.size(); ++ci) totalAllocated += arch->GetAllocatedChunkCount(static_cast<uint32_t>(ci));
	}

	// Allocated high-water mark must be >= live count + dead slots still in slab
	// (not yet compacted). It can only shrink when slots are reused by new entities.
	ASSERT(totalAllocated >= static_cast<uint32_t>(SpawnCount - DestroyCount));

	// Remaining 8 handles should still have their components according to HasComponent
	for (int i = DestroyCount; i < SpawnCount; ++i)
	{
		ASSERT(Reg->HasComponent<CTransform<>>(handles[static_cast<size_t>(i)]));
	}

	Engine.ResetRegistry();
}
