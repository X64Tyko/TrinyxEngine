#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Public/TestEntity.h"
#include "CTransform.h"
#include "EntityRecord.h"

// Validates handle lifecycle: entity creation gives a valid handle; after Destroy +
// ProcessDeferredDestructions the live count drops, and the old handle's record
// reflects the changed generation (stale handle).
//
// API GAP NOTE: Registry has no public IsHandleValid(EntityHandle) method.
// The generation-bump stale-handle check lives on the private GlobalEntityRegistry
// (EntityArchive::IsHandleValid). OOP code outside the engine cannot safely detect
// a stale handle without going through GetRecord() and inspecting generation manually.
// This test documents that gap — Registry::IsHandleValid(EntityHandle) should be
// added to the public API.
TEST(Registry_StaleHandle)
{
	Registry* Reg = Engine.GetRegistry();

	const uint32_t countBefore = Reg->GetTotalEntityCount();

	// Create an entity and save the handle
	EntityHandle handle = Reg->Create<TestEntity<>>();
	ASSERT(handle.IsValid());

	const uint32_t countAfter = Reg->GetTotalEntityCount();
	ASSERT(countAfter == countBefore + 1);

	// Save a copy of the handle — this is the "OOP code holding a reference"
	EntityHandle staleHandle = handle;

	// Destroy the entity
	Reg->Destroy(handle);

	// Before ProcessDeferredDestructions: entity is tombstoned (Active cleared),
	// but the slot hasn't been reclaimed yet. Live count should already decrement
	// because the handle is logically invalid from this point.
	// Immediately process so the generation bump happens.
	Reg->ProcessDeferredDestructions();

	const uint32_t countFinal = Reg->GetTotalEntityCount();
	ASSERT_EQ(countFinal, countBefore);

	// The old handle's record should have a different generation now.
	// GetRecord() gives us the current record at that slot index.
	// If the generation in the slot != staleHandle's stored generation,
	// the handle is stale. This is the manual check OOP code must do today.
	EntityRecord currentRecord = Reg->GetRecord(staleHandle);
	bool generationMismatch    = (staleHandle.GetGeneration() != currentRecord.EntityInfo.GetGeneration());

	// After ProcessDeferredDestructions, FreeGlobalHandle bumps the generation at this slot.
	// The stale handle retains the old generation — they must differ.
	//
	// TODO(api-gap): Add Registry::IsHandleValid(EntityHandle) as a public method
	// so engine users can check this without manual generation comparison.
	ASSERT(generationMismatch);

	// ResetRegistry to leave clean state for subsequent tests
	Engine.ResetRegistry();
}
