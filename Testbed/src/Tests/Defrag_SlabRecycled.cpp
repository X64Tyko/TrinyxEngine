#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Public/TestEntity.h"
#include "Archetype.h"
#include "TemporalComponentCache.h"

// Verifies the slab-recycling path of phase-2 defrag (ComponentCacheBase::TryReuseFreedSlab):
//
// Setup: 4 full chunks (Chunks[0..3]) + 1 pre-allocated spare (Chunks[4]).
// Destroy all entities in chunk 0 (ArchIndex 0..epc-1) → 25% hole ratio.
// ForceDefragSync:
//   - Moves Chunks[3]'s epc entities into chunk 0's holes.
//   - TrimTailChunks frees Chunks[4] (spare) then Chunks[3] (now empty).
//   - NotifyChunkFreed records their slab regions in FreedChunkSlabs.
//
// After defrag, spawn epc new entities:
//   - AllocateChunk → TryReuseFreedSlab finds Chunks[4]'s freed slab (first in list).
//   - New chunk's CacheIndexStart must equal the freed spare's CacheIndexStart.
//   - AllocatedEntityCount and TotalEntityCount remain consistent.
//   - Surviving handles remain valid.
TEST(Defrag_SlabRecycled)
{
	Engine.ResetRegistry();
	Registry* Reg = Engine.GetRegistry();

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

	// 4 full chunks + 1 pre-allocated spare = 5 chunks.
	ASSERT_EQ(arch->Chunks.size(), 5u);
	ASSERT_EQ(arch->AllocatedEntityCount, total);

	// Capture the spare chunk's CacheIndexStart before defrag frees it.
	// TrimTailChunks pops back-to-front: spare (Chunks[4]) is freed first,
	// so its freed slab ends up at FreedChunkSlabs[0] and gets reused first.
	const size_t spareChunkCacheStart = arch->Chunks[4]->Header.CacheIndexStart;

	// Destroy chunk 0's epc entities → 25% hole ratio.
	for (uint32_t i = 0; i < epc; ++i)
		Reg->Destroy(all[i]);

	Reg->ProcessDeferredDestructions();
	Engine.ConfirmLocalRecycles();

	ASSERT_EQ(arch->TotalEntityCount,     3 * epc);
	ASSERT_EQ(arch->AllocatedEntityCount, total);

	// Defrag: moves chunk 3 entities into chunk 0's holes; TrimTailChunks frees
	// Chunks[4] (spare) then Chunks[3] (emptied). NotifyChunkFreed stores both
	// in FreedChunkSlabs. AllocatedEntityCount shrinks to 3*epc.
	Reg->ForceDefragSync();

	ASSERT_EQ(arch->Chunks.size(),        3u);
	ASSERT_EQ(arch->AllocatedEntityCount, 3 * epc);
	ASSERT_EQ(arch->TotalEntityCount,     3 * epc);

	// Spawn epc new entities — PushEntities triggers one AllocateChunk call.
	// TryReuseFreedSlab must pick up the spare chunk's freed slab (first in list)
	// instead of advancing the partition cursor.
	std::vector<EntityHandle> newBatch = Reg->Create<TestEntity<>>(epc);

	// 3 existing chunks + 1 recycled + 1 fresh spare = 5 again.
	ASSERT_EQ(arch->Chunks.size(), 5u);

	// The recycled chunk sits at Chunks[3] (the 4th chunk, 0-indexed).
	ASSERT_EQ(arch->Chunks[3]->Header.CacheIndexStart, spareChunkCacheStart);

	// Entity counts are consistent.
	ASSERT_EQ(arch->TotalEntityCount,     4 * epc);
	ASSERT_EQ(arch->AllocatedEntityCount, 4 * epc);

	// All new entities are valid.
	for (EntityHandle h : newBatch)
		ASSERT(Reg->HasComponent<CTransform<>>(h));

	// Surviving middle-chunk entities (all[epc..3*epc-1]) are still valid
	// after being untouched by defrag.
	for (uint32_t i = epc; i < 3 * epc; ++i)
		ASSERT(Reg->HasComponent<CTransform<>>(all[i]));

	Engine.ResetRegistry();
}
