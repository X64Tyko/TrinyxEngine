#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Public/TestEntity.h"
#include "CTransform.h"
#include "Archetype.h"

// Verifies the entity-slot compactor (DefragSystem phase 1):
//
// Setup: 4 full chunks. Destroy the entire first chunk (ArchIndex 0..epc-1),
// leaving a 25% hole ratio (above the 20% HoleThreshold).
//
// After ForceDefragSync:
//   - The live entities in chunk 3 (highest ArchIndex) are moved into the holes
//     in chunk 0, fully evacuating chunk 3.
//   - TrimTailChunks frees chunk 3: Chunks.size() 4 → 3, AllocatedEntityCount shrinks.
//   - TotalEntityCount remains 3*epc (no entity lost or duplicated).
//   - InactiveEntitySlots becomes empty (all holes filled, freed slots trimmed).
//   - All surviving EntityHandles remain valid after relocation.
//
// EntitiesPerChunk is read from the live archetype so the test is layout-agnostic.
TEST(Defrag_CompactsHoles)
{
	// Start clean so chunk counts are deterministic.
	Engine.ResetRegistry();
	Registry* Reg = Engine.GetRegistry();

	// Spawn one entity to initialise the archetype and discover EntitiesPerChunk.
	EntityHandle probe = Reg->Create<TestEntity<>>();
	std::vector<Archetype*> arches = Reg->ClassQuery<TestEntity<>>();
	ASSERT_EQ(arches.size(), 1u);
	Archetype* arch    = arches[0];
	const uint32_t epc = arch->EntitiesPerChunk;

	// Fill to exactly 4 full chunks.
	const uint32_t total = 4 * epc;
	std::vector<EntityHandle> rest = Reg->Create<TestEntity<>>(total - 1);

	std::vector<EntityHandle> all;
	all.reserve(total);
	all.push_back(probe);
	all.insert(all.end(), rest.begin(), rest.end());

	ASSERT_EQ(arch->Chunks.size(),        5u); //Archetype allocates an extra chunk when full
	ASSERT_EQ(arch->AllocatedEntityCount, total);
	ASSERT_EQ(arch->TotalEntityCount,     total);

	// Destroy all epc entities in chunk 0 (ArchIndex 0..epc-1).
	// Hole ratio = epc / (4*epc) = 25% > HoleThreshold (20%).
	for (uint32_t i = 0; i < epc; ++i)
		Reg->Destroy(all[i]);

	Reg->ProcessDeferredDestructions();
	Engine.ConfirmLocalRecycles();

	ASSERT_EQ(arch->TotalEntityCount,     3 * epc);
	ASSERT_EQ(arch->AllocatedEntityCount, total);  // high-water mark unchanged

	// ForceDefragSync bypasses the 1024-frame cadence and loops internally until
	// the archetype drops below HoleThreshold (handles epc > MaxMovesPerTick).
	// Expected: entities in chunk 3 fill chunk 0's holes; chunk 3 freed.
	Reg->ForceDefragSync();

	// Chunk 3 freed: 4 → 3 chunks, AllocatedEntityCount = 3*epc.
	ASSERT_EQ(arch->Chunks.size(),        3u);
	ASSERT_EQ(arch->AllocatedEntityCount, 3 * epc);

	// Live count must be exactly preserved.
	ASSERT_EQ(arch->TotalEntityCount,     3 * epc);

	// No holes remain.
	ASSERT(arch->InactiveEntitySlots.empty());

	// Surviving handles valid — both the untouched middle entities (chunk 1-2,
	// all[epc..3*epc-1]) and the relocated tail entities (all[3*epc..total-1],
	// now physically in chunk 0 after the move).
	for (uint32_t i = epc; i < total; ++i)
		ASSERT(Reg->HasComponent<CTransform<>>(all[i]));

	Engine.ResetRegistry();
}