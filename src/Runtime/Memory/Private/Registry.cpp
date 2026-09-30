#include "Registry.h"
#include "AssetRegistry.h"
#include "CacheSlotMeta.h"
#include "FieldProxy.h"
#include "JoltPhysics.h"
#include "Profiler.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <immintrin.h>
#include <emmintrin.h>
#include <queue>
#include <span>
#include <vector>
#include "Archetype.h"

#include "CTransform.h"
#include "SchemaReflector.h"

Registry::Registry()
	: NextRecordIndex(1) // Start at 1 (0 is reserved for Invalid)
	  ,
	  NextLocalIndex(1),
	  NextNetIndex(1)
{
	TNX_ZONE_N("Registry::Constructor");
}

Registry::Registry(const EngineConfig* config)
	: Registry()
{
#ifdef TNX_ENABLE_ROLLBACK
	HistorySlab.Initialize(config); // Temporal: config->TemporalFrameCount frames, rollback-capable
#endif
	VolatileSlab.Initialize(config); // Volatile: 5 frames, no rollback
	InitializeArchetypes();
}

Registry::~Registry()
{
	LOG_ENG_INFO_F("Destroying Registry with %zu archetypes", Archetypes.size());

	for (auto& Pair : Archetypes)
	{
		LOG_ENG_INFO_F("Deleting archetype with %zu chunks", Pair.second->Chunks.size());
		delete Pair.second;
		LOG_ENG_INFO("Archetype deleted successfully");
	}
	Archetypes.clear();
}

// Bridge GHandle → LHandle: allocates a local index from a separate index space,
// wires LocalToRecord so LHandle can resolve back to the record, and stores the
// LHandle on the record itself so the record knows its OOP-facing identity.
EntityHandle Registry::MakeEntityHandle(GlobalEntityHandle gHandle, ClassID classID)
{
	EntityHandle lHandle;
	lHandle.HandleIndex = AllocateLocalIndex();
	lHandle.ClassType   = classID;

	GlobalEntityRegistry.LocalToRecord.set(lHandle.GetHandleIndex(), gHandle);

	EntityRecord* record = GlobalEntityRegistry.Records[gHandle.GetIndex()];
	if (record)
	{
		lHandle.Generation = record->GetGeneration();
		record->LHandle    = lHandle;
	}

	return lHandle;
}

EntityHandle Registry::CreateByClassID(ClassID classID)
{
	GlobalEntityHandle GHandle;
	CreateInternal(classID, { &GHandle, 1 });
	return MakeEntityHandle(GHandle, classID);
}

std::vector<EntityHandle> Registry::CreateByClassID(ClassID classID, size_t count)
{
	std::vector<GlobalEntityHandle> GHandles(count);
	CreateInternal(classID, GHandles);

	std::vector<EntityHandle> handles(count);
	for (size_t i = 0; i < count; ++i)
		handles[i] = MakeEntityHandle(GHandles[i], classID);
	return handles;
}

void Registry::Recreate(EntityHandle& inHandle)
{
	if (GlobalEntityRegistry.IsHandleValid(inHandle)) Destroy(inHandle);
	inHandle = CreateByClassID(inHandle.GetTypeID());
}

void Registry::RecreateAs(EntityHandle& inHandle, ClassID newClassID)
{
	if (GlobalEntityRegistry.IsHandleValid(inHandle)) Destroy(inHandle);
	inHandle = CreateByClassID(newClassID > 0 ? newClassID : inHandle.GetTypeID());
}

void Registry::RecreateAs(EntityHandle& inHandle, const EntityHandle& asHandle)
{
	if (GlobalEntityRegistry.IsHandleValid(inHandle)) Destroy(inHandle);
	ClassID classID = asHandle.GetTypeID() != inHandle.GetTypeID()
						  ? asHandle.GetTypeID()
						  : inHandle.GetTypeID();
	inHandle        = CreateByClassID(classID);
}

Archetype* Registry::GetOrCreateArchetype(const Signature& sig, const ClassID& id)
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);
	auto key = Archetype::ArchetypeKey(sig, id);

	auto It = Archetypes.find(key);
	if (It)
	{
		return *It;
	}

	auto NewArchetype = new Archetype(sig, id);

	{
		const auto& cfr              = ReflectionRegistry::Get();
		const std::string* debugName = nullptr;
		for (const auto& entry : cfr.NameToClassID)
		{
			if (entry.second == id)
			{
				debugName = &entry.first;
				break;
			}
		}
		if (debugName) NewArchetype->DebugName = debugName->c_str();
	}

	std::vector<ComponentMetaEx> Components;
	auto& MR = ReflectionRegistry::Get();

	auto compListIt = MR.ClassToComponentList.find(id);
	if (compListIt != MR.ClassToComponentList.end())
	{
		auto& CFR = ReflectionRegistry::Get();

		for (ComponentTypeID compTypeID : compListIt->second)
		{
			Components.push_back(CFR.GetComponentMeta(compTypeID));
		}
	}

	SystemID sysID = SystemID::None;
	auto sysIt     = MR.ClassSystemID.find(id);
	if (sysIt != MR.ClassSystemID.end()) sysID = sysIt->second;

	NewArchetype->BuildLayout(this, Components, sysID);

	Archetypes[key] = NewArchetype;
	return NewArchetype;
}

// =============================================================================
// Handle allocation and recycling
// =============================================================================
//
// Three index spaces are managed independently:
//   Record indices  — GHandle.Index, recycled immediately on free
//   Local indices   — LHandle.HandleIndex, deferred via PendingLocalRecycles
//   Net indices     — NetHandle.NetIndex, deferred via PendingNetRecycles
//
// Local/net indices use deferred recycling to prevent ABA problems:
// a stale handle held by OOP code or a remote client could alias a newly
// created entity if the index were reused immediately. The pending lists
// hold freed indices until ConfirmLocalRecycles/ConfirmNetRecycles is called.

GlobalEntityHandle Registry::AllocateGlobalHandle()
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);

	GlobalEntityHandle GHandle;
	GHandle.Value = 0;

	if (!FreeRecordIndices.empty())
	{
		uint32_t Index = FreeRecordIndices.front();
		FreeRecordIndices.pop();

		// Bump generation so stale GHandles from the previous occupant won't validate
		EntityRecord& Record = GlobalEntityRegistry.Records.findOrAdd(Index);
		if (Record.IsValid())
		{
			LOG_ENG_ERROR_F("Existing entity requested at index: %u", Index);
			assert(false && "Reallocating entity record");
		}

		uint16_t Generation = Record.GetGeneration() + 1;
		if (Generation == 0) Generation = 1; // skip 0 (reserved for invalid)

		GHandle.Index      = Index;
		GHandle.Generation = Generation;
	}
	else
	{
		GHandle.Index      = NextRecordIndex++;
		GHandle.Generation = 1;
	}

	return GHandle;
}

void Registry::FreeGlobalHandle(GlobalEntityHandle gHandle)
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);

	uint32_t index = gHandle.GetIndex();

	EntityRecord* record = GlobalEntityRegistry.Records[index];
	if (!record || !record->IsValid())
	{
		LOG_ENG_WARN_F("Invalid entity record at index %u", index);
		return;
	}

	ComponentCacheBase* cache  = GetTemporalCache();
	TemporalFrameHeader* hdr   = cache->GetFrameHeader();
	const ComponentTypeID slot = CacheSlotMeta<>::StaticTemporalIndex();
	auto* flags                = static_cast<int32_t*>(cache->GetFieldData(hdr, slot, 0));
	if (flags)
	{
		flags[record->CacheEntityIndex] &= ~static_cast<int32_t>(TemporalFlagBits::Tombstone);
	}

	// Defer local/net index recycling — they stay in pending until confirmed safe
	if (record->LHandle.IsValid()) RequestLocalRecycle(record->LHandle.GetHandleIndex());
	if (record->NetworkID.GetHandleIndex() > 0) RequestNetRecycle(record->NetworkID.GetHandleIndex());

	// Record index goes straight back to the free pool (generation bump prevents stale access)
	FreeRecordIndices.push(index);

	record->Arch                = nullptr;
	record->TargetChunk         = nullptr;
	record->EntityInfo.ValidBit = false;
}

// --- Local handle index allocation (OOP land) ---

uint32_t Registry::AllocateLocalIndex()
{
	if (!FreeLocalIndices.empty())
	{
		uint32_t Index = FreeLocalIndices.front();
		FreeLocalIndices.pop();
		return Index;
	}
	return NextLocalIndex++;
}

void Registry::RequestLocalRecycle(uint32_t localIndex)
{
	PendingLocalRecycles.push_back(localIndex);
}

void Registry::ConfirmLocalRecycles()
{
	for (uint32_t Index : PendingLocalRecycles)
		FreeLocalIndices.push(Index);
	PendingLocalRecycles.clear();
}

void Registry::ConfirmTombstone(uint32_t recordIndex)
{
	auto it = std::find(TombstoneRecordIndices.begin(), TombstoneRecordIndices.end(), recordIndex);
	if (it == TombstoneRecordIndices.end())
	{
		LOG_ENG_WARN_F("ConfirmTombstone: record index %u not found in tombstone list", recordIndex);
		return;
	}

	EntityRecord* record = GlobalEntityRegistry.Records[recordIndex];
	if (!record || !record->IsValid())
	{
		LOG_ENG_WARN_F("ConfirmTombstone: record at index %u is invalid", recordIndex);
		TombstoneRecordIndices.erase(it);
		return;
	}

	ComponentCacheBase* cache  = GetTemporalCache();
	TemporalFrameHeader* hdr   = cache->GetFrameHeader();
	const ComponentTypeID slot = CacheSlotMeta<>::StaticTemporalIndex();
	auto* flags                = static_cast<int32_t*>(cache->GetFieldData(hdr, slot, 0));
	if (flags)
	{
		flags[record->CacheEntityIndex] &= ~static_cast<int32_t>(TemporalFlagBits::Tombstone);
		flags[record->CacheEntityIndex] |= static_cast<int32_t>(TemporalFlagBits::Dirty | TemporalFlagBits::DirtiedFrame);
	}

	GlobalEntityHandle gHandle;
	gHandle.Index      = recordIndex;
	gHandle.Generation = record->GetGeneration();

	PendingConfirmedDestructions.push_back(gHandle);
	TombstoneRecordIndices.erase(it);
}

bool Registry::IsTombstoned(uint32_t recordIndex) const
{
	return std::find(TombstoneRecordIndices.begin(), TombstoneRecordIndices.end(), recordIndex)
		   != TombstoneRecordIndices.end();
}

// --- Net handle index allocation ---

uint32_t Registry::AllocateNetIndex()
{
	if (!FreeNetIndices.empty())
	{
		uint32_t Index = FreeNetIndices.front();
		FreeNetIndices.pop();
		return Index;
	}
	return NextNetIndex++;
}

void Registry::RequestNetRecycle(uint32_t netIndex)
{
	PendingNetRecycles.push_back(netIndex);
}

void Registry::ConfirmNetRecycles()
{
	for (uint32_t Index : PendingNetRecycles)
		FreeNetIndices.push(Index);
	PendingNetRecycles.clear();
}

// Core creation: allocates GHandles and populates EntityRecords.
// Does NOT allocate local or net handles — that's the caller's responsibility
// via MakeEntityHandle (for local) or a future AssignNetHandle (for network).
void Registry::CreateInternal(ClassID classID, std::span<GlobalEntityHandle> outHandles)
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);

	const size_t count = outHandles.size();
	auto& MR           = ReflectionRegistry::Get();
	Signature sig      = MR.ClassToArchetype[classID];

#ifdef _DEBUG
	if (MR.ClassToArchetype.find(classID) == MR.ClassToArchetype.end())
	{
		const char* name = (classID < 4096) ? MR.EntityGetters[classID].Name : nullptr;
		LOG_ENG_ERROR_F("CreateInternal: ClassID %u ('%s') not registered",
			classID, name ? name : "unknown");
		assert(false && "Entity ClassID not registered");
		return;
	}
#endif

	Archetype* arch = GetOrCreateArchetype(sig, classID);

	std::vector<Archetype::EntitySlot> Slots(count);
	arch->PushEntities(Slots);

	for (size_t i = 0; i < count; ++i)
	{
		Archetype::EntitySlot Slot = Slots[i];
		GlobalEntityHandle GHandle = AllocateGlobalHandle();

		EntityRecord& Record         = GlobalEntityRegistry.Records.findOrAdd(GHandle.GetIndex());
		Record.Arch                  = arch;
		Record.TargetChunk           = Slot.TargetChunk;
		Record.ArchIndex             = Slot.ArchIndex;
		Record.LocalIndex            = Slot.LocalIndex;
		Record.ChunkIndex            = Slot.ChunkIndex;
		Record.CacheEntityIndex      = Slot.CacheIndex;
		Record.EntityInfo.Generation = GHandle.GetGeneration();
		Record.EntityInfo.ValidBit   = true;

		GlobalEntityRegistry.CacheToRecord.set(Slot.CacheIndex, GHandle);

		outHandles[i] = GHandle;
	}
}

// Resolves LHandle → GHandle via LocalToRecord, then marks the entity as tombstoned.
// Actual cleanup happens after ConfirmTombstone + ProcessDeferredDestructions.
void Registry::Destroy(EntityHandle lHandle)
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);

	GlobalEntityHandle gHandle = GlobalEntityRegistry.LookupGlobalHandle(lHandle);
	uint32_t index             = gHandle.GetIndex();
	if (index == 0) return;

	EntityRecord* record = GlobalEntityRegistry.Records[index];
	if (!record || !record->IsValid()) return;

	ComponentCacheBase* cache  = GetTemporalCache();
	TemporalFrameHeader* hdr   = cache->GetFrameHeader();
	const ComponentTypeID slot = CacheSlotMeta<>::StaticTemporalIndex();
	auto* flags                = static_cast<int32_t*>(cache->GetFieldData(hdr, slot, 0));
	if (flags)
	{
		flags[record->CacheEntityIndex] |= static_cast<int32_t>(TemporalFlagBits::Tombstone | TemporalFlagBits::Dirty | TemporalFlagBits::DirtiedFrame);
		flags[record->CacheEntityIndex] &= ~static_cast<int32_t>(TemporalFlagBits::Active);
	}

	TombstoneRecordIndices.push_back(index);
}

void Registry::DestroyByGlobalHandle(GlobalEntityHandle gHandle)
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);
	uint32_t index = gHandle.GetIndex();
	if (index == 0) return;

	EntityRecord* record = GlobalEntityRegistry.Records[index];
	if (!record || !record->IsValid()) return;

	ComponentCacheBase* cache  = GetTemporalCache();
	TemporalFrameHeader* hdr   = cache->GetFrameHeader();
	const ComponentTypeID slot = CacheSlotMeta<>::StaticTemporalIndex();
	auto* flags                = static_cast<int32_t*>(cache->GetFieldData(hdr, slot, 0));
	if (flags)
	{
		flags[record->CacheEntityIndex] |= static_cast<int32_t>(TemporalFlagBits::Tombstone | TemporalFlagBits::Dirty | TemporalFlagBits::DirtiedFrame);
	}

	TombstoneRecordIndices.push_back(index);
}

void Registry::ForceDestroyByGlobalHandle(GlobalEntityHandle gHandle)
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);
	uint32_t index = gHandle.GetIndex();
	if (index == 0) return;

	EntityRecord* record = GlobalEntityRegistry.Records[index];
	if (!record || !record->IsValid()) return;

	PendingConfirmedDestructions.push_back(gHandle);
}

bool Registry::DestroyRecord(GlobalEntityHandle& gHandle)
{
	EntityRecord* record = GlobalEntityRegistry.Records[gHandle.GetIndex()];
	if (!record)
	{
		LOG_ENG_ERROR_F("Failed to find record for handle %u during destruction", gHandle.GetIndex());
		return false;
	}
	Archetype* arch = record->Arch;

	// Remove from archetype
	arch->RemoveEntity(record->ChunkIndex, record->LocalIndex, record->ArchIndex);
	return true;
}

bool Registry::DestroyRecord(EntityRecord& record)
{
	if (!record.IsValid())
	{
		LOG_ENG_ERROR("Requested record is invalid for destruction");
		return false;
	}
	Archetype* arch = record.Arch;

	// Remove from archetype
	arch->RemoveEntity(record.ChunkIndex, record.LocalIndex, record.ArchIndex);
	return true;
}

// Processes all confirmed destructions (moved from TombstoneRecordIndices via ConfirmTombstone).
// Generation check prevents double-free if the same GHandle was queued twice
// or the slot was already recycled by a prior frame's destruction.
void Registry::ProcessDeferredDestructions()
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);

	// Confirm all pending tombstones (needed for single-player/co-op where replication system doesn't run)
	{
		std::vector<uint32_t> tombstones = std::move(TombstoneRecordIndices);
		TombstoneRecordIndices.clear();
		for (uint32_t recordIndex : tombstones)
		{
			EntityRecord* record = GlobalEntityRegistry.Records[recordIndex];
			if (!record || !record->IsValid())
			{
				LOG_ENG_WARN_F("ProcessDeferredDestructions: record at index %u is invalid", recordIndex);
				continue;
			}

			// Check if this entity is replicated — if so, leave it for the ReplicationSystem
			ComponentCacheBase* cache  = GetTemporalCache();
			TemporalFrameHeader* hdr   = cache->GetFrameHeader();
			const ComponentTypeID slot = CacheSlotMeta<>::StaticTemporalIndex();
			auto* flags                = static_cast<int32_t*>(cache->GetFieldData(hdr, slot, 0));
			if (flags)
			{
				// If replication is active, leave replicated entities for the ReplicationSystem
				if (ReplicationActive)
				{
					const bool isReplicated = (flags[record->CacheEntityIndex] & static_cast<int32_t>(TemporalFlagBits::Replicated)) != 0;
					if (isReplicated)
					{
						// Put it back into TombstoneRecordIndices for the ReplicationSystem to handle
						TombstoneRecordIndices.push_back(recordIndex);
						continue;
					}
				}

				// Clear Tombstone flag in the cache slab
				flags[record->CacheEntityIndex] &= ~static_cast<int32_t>(TemporalFlagBits::Tombstone);
				flags[record->CacheEntityIndex] |= static_cast<int32_t>(TemporalFlagBits::Dirty | TemporalFlagBits::DirtiedFrame);
			}

			GlobalEntityHandle gHandle;
			gHandle.Index      = recordIndex;
			gHandle.Generation = record->GetGeneration();

			PendingConfirmedDestructions.push_back(gHandle);
		}
	}

	if (PendingConfirmedDestructions.empty()) [[likely]]
		return;

	for (GlobalEntityHandle GHandle : PendingConfirmedDestructions)
	{
		EntityRecord* Record = GlobalEntityRegistry.Records[GHandle.GetIndex()];

		if (!Record->IsValid()) continue;
		if (Record->GetGeneration() != GHandle.GetGeneration()) continue;

		// Checkin any asset-ref fields before the slot is reclaimed.
		{
			void* FieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
			Record->Arch->BuildFieldArrayTable(
				Record->TargetChunk, FieldArrayTable,
				GetTemporalCache()->GetActiveWriteFrame(),
				GetVolatileCache()->GetActiveWriteFrame());

			for (auto& [key, fdesc] : Record->Arch->ArchetypeFieldLayout)
			{
				if (fdesc.refAssetType == AssetType::Invalid) continue;

				auto* arr     = static_cast<uint32_t*>(FieldArrayTable[fdesc.fieldSlotIndex]);
				uint32_t slot = arr[Record->LocalIndex];
				if (slot == 0) continue;

				AssetRegistry::Get().CheckinBySlot(fdesc.refAssetType, slot, &arr[Record->LocalIndex]);
			}
		}

		if (DestroyRecord(*Record))
		{
			FreeGlobalHandle(GHandle);
		}
	}

	PendingConfirmedDestructions.clear();
}

EntityRecord Registry::GetRecordByCache(EntityCacheHandle cacheHandle) const
{
	GlobalEntityHandle gHandle = GlobalEntityRegistry.LookupGlobalHandle(cacheHandle);
	EntityRecord record        = GlobalEntityRegistry.Records[gHandle.GetIndex()];
	if (!record.IsValid() || record.GetGeneration() != gHandle.GetGeneration()) return EntityRecord{};
	return record;
}

EntityRecord Registry::GetRecord(EntityHandle handle) const
{
	GlobalEntityHandle gHandle = GlobalEntityRegistry.LookupGlobalHandle(handle);
	EntityRecord record        = GlobalEntityRegistry.Records[gHandle.GetIndex()];
	if (!record.IsValid() || record.GetGeneration() != gHandle.GetGeneration()) return EntityRecord{};
	return record;
}

GlobalEntityHandle Registry::FindEntityByLocation(EntityCacheHandle cacheHandle) const
{
	GlobalEntityHandle gHandle = GlobalEntityRegistry.LookupGlobalHandle(cacheHandle);
	EntityRecord record        = GlobalEntityRegistry.Records[gHandle.GetIndex()];
	if (!record.IsValid() || record.GetGeneration() != gHandle.GetGeneration()) return GlobalEntityHandle();
	return gHandle;
}

void Registry::InitializeArchetypes()
{
	auto& MR = ReflectionRegistry::Get();

	for (auto& Arch : MR.ClassToArchetype)
	{
		auto key            = Archetype::ArchetypeKey(Arch.second, Arch.first);
		Archetype*& NewArch = Archetypes[key];
		if (!NewArch)
		{
			NewArch = new Archetype(key);

			{
				const auto& cfr              = ReflectionRegistry::Get();
				const std::string* debugName = nullptr;
				for (const auto& entry : cfr.NameToClassID)
				{
					if (entry.second == key.ID)
					{
						debugName = &entry.first;
						break;
					}
				}
				if (debugName) NewArch->DebugName = debugName->c_str();
			}

			std::vector<ComponentMetaEx> Components;
			for (auto& CompID : MR.ClassToComponentList[Arch.first])
			{
				Components.push_back(MR.GetComponentMeta(CompID));
			}
			NewArch->BuildLayout(this, Components, MR.ClassSystemID[Arch.first]);
		}
	}
}

void Registry::PropagateFrame(uint32_t currentFrame, bool bPreserveDirtiedFrame)
{
	TNX_ZONE_NC("Propagating Frame", TNX_COLOR_LOGIC)

	TrinyxJobs::JobCounter PropagationCounter;
#ifdef TNX_ENABLE_ROLLBACK
	if (bPreserveDirtiedFrame)
		// Scatter-copy only entities with DirtiedFrameBit; non-dirty entities in the
		// destination slot retain their original-timeline data (no full memcpy).
		HistorySlab.PropagateFrameResim(PropagationCounter);
	else
		HistorySlab.PropagateFrame(PropagationCounter);
#endif

	// Volatile cache: uses triple-buffer strategy with lock-based frame selection
	// (defined in ComponentCache<Volatile>)
	if (!bPreserveDirtiedFrame)
		VolatileSlab.PropagateFrame(PropagationCounter);

	TrinyxJobs::WaitForCounter(&PropagationCounter, TrinyxJobs::Queue::Logic);

	// ── Post-propagation dirty bit maintenance ──────────────────────────
	// Skipped during rollback resim (bPreserveDirtiedFrame=true) so DirtiedFrame (bit 29)
	// accumulates across all resim steps and marks every entity touched by the resimulation.
	// After resim completes the normal path resumes and clears the accumulated bits.
	if (!bPreserveDirtiedFrame)
	{
		TNX_ZONE_NC("Clear Dirty Bits", TNX_COLOR_LOGIC)

		const bool renderCaughtUp = RenderHasAcked && RenderAck.load(std::memory_order_acquire) >= LastPublishedFrame;
		const int32_t clearMask   = renderCaughtUp
										? ~(static_cast<int32_t>(TemporalFlagBits::Dirty) | static_cast<int32_t>(TemporalFlagBits::DirtiedFrame))
										: ~static_cast<int32_t>(TemporalFlagBits::DirtiedFrame);

		// CacheSlotMeta (flags) is always in the temporal cache (or volatile when rollback is off).
		// Get the flags field from the active write frame.
		ComponentCacheBase* flagsCache  = GetTemporalCache();
		TemporalFrameHeader* writeHdr   = flagsCache->GetFrameHeader();
		const ComponentTypeID flagsSlot = CacheSlotMeta<>::StaticTemporalIndex();
		auto* flags                     = static_cast<int32_t*>(flagsCache->GetFieldData(writeHdr, flagsSlot, 0));

		if (flags)
		{
			// MAX_CACHED_ENTITIES worth of int32_t flags in the slab.
			// Iterate the full range — bitplane scan over gaps costs ~microseconds.
			const size_t entityCount = flagsCache->GetMaxCachedEntityCount();
			using FlagTraits         = SIMDTraits<int32_t, FieldWidth::Wide>;
			using FlagVec            = typename FlagTraits::VecType;
			const size_t stride      = kSIMDWide32Lanes;
			const size_t simdCount   = entityCount / stride;
			const size_t remainder   = entityCount % stride;

			const FlagVec vMask = FlagTraits::set1(clearMask);
			for (size_t i = 0; i < simdCount; ++i)
			{
				auto* p = flags + i * stride;
				FlagTraits::store(p, WideMaskType{}, FlagTraits::bitand_(FlagTraits::load(p), vMask));
			}
			for (size_t i = simdCount * stride; i < simdCount * stride + remainder; ++i)
			{
				flags[i] &= clearMask;
			}
		}
	}
}

#ifdef TNX_ENABLE_ROLLBACK
void Registry::ClearDirtiedFrameBits()
{
	ComponentCacheBase* flagsCache  = GetTemporalCache();
	TemporalFrameHeader* writeHdr   = flagsCache->GetFrameHeader();
	const ComponentTypeID flagsSlot = CacheSlotMeta<>::StaticTemporalIndex();
	auto* flags                     = static_cast<int32_t*>(flagsCache->GetFieldData(writeHdr, flagsSlot, 0));

	if (!flags) return;

	const size_t entityCount = flagsCache->GetMaxCachedEntityCount();
	using FlagTraits         = SIMDTraits<int32_t, FieldWidth::Wide>;
	using FlagVec            = typename FlagTraits::VecType;
	const size_t stride      = kSIMDWide32Lanes;
	const size_t simdCount   = entityCount / stride;
	const size_t remainder   = entityCount % stride;
	const int32_t clearMask  = ~static_cast<int32_t>(TemporalFlagBits::DirtiedFrame);
	const FlagVec vMask      = FlagTraits::set1(clearMask);

	for (size_t i = 0; i < simdCount; ++i)
	{
		auto* p = flags + i * stride;
		FlagTraits::store(p, WideMaskType{}, FlagTraits::bitand_(FlagTraits::load(p), vMask));
	}
	for (size_t i = simdCount * stride; i < simdCount * stride + remainder; ++i)
	{
		flags[i] &= clearMask;
	}
}
#endif

// Hard reset — wipes all entities, handles, free lists, caches, and archetype data.
// Skips the normal deferred destruction path. Used by tests.
void Registry::ResetRegistry()
{
	GlobalEntityRegistry.Records.clear_all();
	GlobalEntityRegistry.NetToRecord.clear_all();
	GlobalEntityRegistry.LocalToRecord.clear_all();
	GlobalEntityRegistry.CacheToRecord.clear_all();

	while (!FreeRecordIndices.empty())
		FreeRecordIndices.pop();
	while (!FreeLocalIndices.empty())
		FreeLocalIndices.pop();
	while (!FreeNetIndices.empty())
		FreeNetIndices.pop();
	PendingLocalRecycles.clear();
	PendingNetRecycles.clear();
	TombstoneRecordIndices.clear();
	PendingConfirmedDestructions.clear();

	NextRecordIndex = 1;
	NextLocalIndex  = 1;
	NextNetIndex    = 1;

	if (PhysicsPtr) PhysicsPtr->ResetAllBodies();

#ifdef TNX_ENABLE_ROLLBACK
	HistorySlab.ResetAllocators();
	HistorySlab.ClearFrameData();
#endif
	VolatileSlab.ResetAllocators();
	VolatileSlab.ClearFrameData();

	for (auto& arch : Archetypes)
		arch.second->FreeAllChunks();
}

int Registry::SweepAliveFlagsToActive()
{
	ComponentCacheBase* cache  = GetTemporalCache();
	const uint32_t frame       = cache->GetActiveWriteFrame();
	TemporalFrameHeader* hdr   = cache->GetFrameHeader(frame);
	const ComponentTypeID slot = CacheSlotMeta<>::StaticTemporalIndex();
	auto* flags                = static_cast<int32_t*>(cache->GetFieldData(hdr, slot, 0));
	if (!flags) return 0;

	const uint32_t max             = cache->GetMaxCachedEntityCount();
	const uint32_t aliveBit        = static_cast<uint32_t>(TemporalFlagBits::Alive);
	const uint32_t activeBit       = static_cast<uint32_t>(TemporalFlagBits::Active);
	const uint32_t tombstoneBit    = static_cast<uint32_t>(TemporalFlagBits::Tombstone);
	const uint32_t dirtyBit        = static_cast<uint32_t>(TemporalFlagBits::Dirty);
	const uint32_t dirtiedFrameBit = static_cast<uint32_t>(TemporalFlagBits::DirtiedFrame);
	const uint32_t aliveShift      = TNX_CTZ32(aliveBit);
	const uint32_t activeShift     = TNX_CTZ32(activeBit);
	const uint32_t tombstoneShift  = TNX_CTZ32(tombstoneBit);

	using Traits               = SIMDTraits<int32_t, FieldWidth::Wide>;
	using VecType              = typename Traits::VecType;
	const VecType vAlive       = Traits::set1(static_cast<int32_t>(aliveBit));
	const VecType vActiveDirty = Traits::set1(static_cast<int32_t>(activeBit | dirtyBit | dirtiedFrameBit));
	const VecType vZero        = Traits::set1(0);
	const VecType vTombstone   = Traits::set1(static_cast<int32_t>(tombstoneBit));
	VecType vCount             = vZero;
	const uint32_t stride      = static_cast<uint32_t>(kSIMDWide32Lanes);
	const uint32_t wideMax     = max & ~(stride - 1u);
	for (uint32_t i = 0; i < wideMax; i += stride)
	{
		const VecType f = Traits::load(flags + i);
		// Skip tombstoned entities
		VecType isTombstone = Traits::srl(Traits::bitand_(f, vTombstone), tombstoneShift);
		VecType shift       = Traits::srl(Traits::bitand_(f, vAlive), aliveShift); // 0 or 1
		VecType neg         = Traits::sub(vZero, shift);                           // 0 or 0xFFFFFFFF
		VecType toSet       = Traits::bitand_(vActiveDirty, neg);                  // activeBit | dirtyBit | dirtiedFrameBit or 0
		// Only set active if not tombstoned
		toSet  = Traits::bitandnot(isTombstone, toSet);
		vCount = Traits::add(vCount, Traits::srl(Traits::bitandnot(f, toSet), activeShift));
		Traits::store(flags + i, WideMaskType{}, Traits::bitor_(f, toSet));
	}

	int sweepCount = Traits::hsum(vCount);

	for (uint32_t i = wideMax; i < max; ++i)
	{
		const uint32_t f    = static_cast<uint32_t>(flags[i]);
		const uint32_t mask = -((f & aliveBit) >> aliveShift);
		// Skip tombstoned entities
		const bool isTombstone = (f & tombstoneBit) != 0;
		if (!isTombstone)
		{
			sweepCount += static_cast<int>((activeBit & mask & ~f) >> activeShift);
			flags[i] = static_cast<int32_t>(f | ((activeBit | dirtyBit | dirtiedFrameBit) & mask));
		}
	}

	return sweepCount;
}

uint32_t Registry::GetTotalChunkCount() const
{
	uint32_t totalChunks = 0;
	for (const auto& [sig, archetype] : Archetypes)
	{
		totalChunks += static_cast<uint32_t>(archetype->Chunks.size());
	}
	return totalChunks;
}

uint32_t Registry::GetTotalEntityCount() const
{
	uint32_t totalEntities = 0;
	for (const auto& [sig, archetype] : Archetypes)
	{
		totalEntities += archetype->TotalEntityCount;
	}
	return totalEntities;
}

#ifdef TNX_ENABLE_ROLLBACK
bool Registry::CheckAndCorrectEntityTransform(const EntityTransformCorrection& correction)
{
	EntityNetHandle netHandle{};
	netHandle.Value = correction.NetHandle;

	GlobalEntityHandle gHandle = GlobalEntityRegistry.LookupGlobalHandle(netHandle);
	if (gHandle.GetIndex() == 0) return false;

	EntityRecord* record = GlobalEntityRegistry.Records[gHandle.GetIndex()];
	if (!record || !record->IsValid()) return false;

	Archetype* arch   = record->Arch;
	Chunk* chunk      = record->TargetChunk;
	uint32_t localIdx = record->LocalIndex;

	void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
	arch->BuildFieldArrayTable(chunk, fieldArrayTable,
		GetTemporalCache()->GetActiveWriteFrame(),
		GetVolatileCache()->GetActiveWriteFrame());

	// Read the resimmed position from the current write frame
	constexpr SimFloat kThresholdSq = SimFloat(0.01f * 0.01f); // 1cm
	SimFloat predictedX = SimFloat(0.f), predictedY = SimFloat(0.f), predictedZ = SimFloat(0.f);
	for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
	{
		if (fdesc.componentID != CTransform<>::StaticTypeID()) continue;
		void* base = fieldArrayTable[fdesc.fieldSlotIndex];
		if (!base) continue;
		auto* fa = static_cast<SimFloat*>(base);
		switch (fdesc.componentSlotIndex)
		{
			case 0:
				predictedX = fa[localIdx];
				break;
			case 1:
				predictedY = fa[localIdx];
				break;
			case 2:
				predictedZ = fa[localIdx];
				break;
			default: break;
		}
	}

	const SimFloat dx = predictedX - correction.PosX;
	const SimFloat dy = predictedY - correction.PosY;
	const SimFloat dz = predictedZ - correction.PosZ;
	if (dx * dx + dy * dy + dz * dz <= kThresholdSq) return false;

	// Still divergent after resim — write server-authoritative transform.
	WriteEntityTransformFields(fieldArrayTable, arch, localIdx,
		correction.PosX, correction.PosY, correction.PosZ,
		correction.RotQx, correction.RotQy, correction.RotQz, correction.RotQw);

	return true;
}
#endif

void Registry::WriteEntityTransformFields(void* const* fieldArrayTable, const Archetype* arch,
	uint32_t localIdx,
	SimFloat posX, SimFloat posY, SimFloat posZ,
	SimFloat rotQx, SimFloat rotQy, SimFloat rotQz, SimFloat rotQw)
{
	for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
	{
		if (fdesc.componentID != CTransform<>::StaticTypeID()) continue;
		void* base = fieldArrayTable[fdesc.fieldSlotIndex];
		if (!base) continue;
		auto* fa = static_cast<SimFloat*>(base);
		switch (fdesc.componentSlotIndex)
		{
			case 0: fa[localIdx] = posX; break;
			case 1: fa[localIdx] = posY; break;
			case 2: fa[localIdx] = posZ; break;
			case 3: fa[localIdx] = rotQx; break;
			case 4: fa[localIdx] = rotQy; break;
			case 5: fa[localIdx] = rotQz; break;
			case 6: fa[localIdx] = rotQw; break;
			default: break;
		}
	}

	auto* flagsArr = static_cast<int32_t*>(fieldArrayTable[0]);
	if (flagsArr)
		flagsArr[localIdx] |= static_cast<int32_t>(TemporalFlagBits::Dirty)
							  | static_cast<int32_t>(TemporalFlagBits::DirtiedFrame);
}

#ifdef TNX_ENABLE_ROLLBACK
void Registry::PushEntityReinitEvent(GlobalEntityHandle gHandle, uint32_t frame)
{
	EntityRecord* rec = GlobalEntityRegistry.Records[gHandle.GetIndex()];
	if (!rec || !rec->IsValid()) return;

	Archetype* arch   = rec->Arch;
	uint32_t localIdx = rec->LocalIndex;

	// Snapshot every SoA field at the current write frame.
	// Stored as (FieldKey, raw bytes) pairs — one allocation per field, small enough
	// for static level geometry (typically 5-7 fields per entity).
	struct FieldSnap
	{
		Archetype::FieldKey key;
		std::vector<uint8_t> data;
	};
	std::vector<FieldSnap> snaps;
	snaps.reserve(arch->ArchetypeFieldLayout.count());

	void* table[MAX_FIELDS_PER_ARCHETYPE] = {};
	arch->BuildFieldArrayTable(rec->TargetChunk, table,
		GetTemporalCache()->GetActiveWriteFrame(),
		GetVolatileCache()->GetActiveWriteFrame());

	for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
	{
		void* base = table[fdesc.fieldSlotIndex];
		if (!base) continue;
		const uint8_t* src = static_cast<const uint8_t*>(base) + localIdx * fdesc.fieldSize;
		snaps.push_back({ fkey, { src, src + fdesc.fieldSize } });
	}

	// Captured from the current write frame: the effect is already in the ring at `frame`.
	ServerEvents.push_back({ frame, [this, gHandle, arch, snaps = std::move(snaps)]() mutable
	{
		EntityRecord* r = GlobalEntityRegistry.Records[gHandle.GetIndex()];
		if (!r || !r->IsValid()) return;

		void* t[MAX_FIELDS_PER_ARCHETYPE] = {};
		arch->BuildFieldArrayTable(r->TargetChunk, t,
			GetTemporalCache()->GetActiveWriteFrame(),
			GetVolatileCache()->GetActiveWriteFrame());

		for (const auto& snap : snaps)
		{
			const auto* fdesc = arch->ArchetypeFieldLayout.find(snap.key);
			if (!fdesc) continue;
			void* base = t[fdesc->fieldSlotIndex];
			if (!base) continue;
			uint8_t* dst = static_cast<uint8_t*>(base) + r->LocalIndex * fdesc->fieldSize;
			std::memcpy(dst, snap.data.data(), snap.data.size());
		}

		// The snapshot memcpy restores the flags field to its snapshotted state, which may
		// have had Dirty/DirtiedFrame cleared. Re-assert both so the render and sweep see it.
		auto* flagsArr = static_cast<int32_t*>(t[0]);
		if (flagsArr)
			flagsArr[r->LocalIndex] |= static_cast<int32_t>(TemporalFlagBits::Dirty)
									   | static_cast<int32_t>(TemporalFlagBits::DirtiedFrame);
	}, true });
}

void Registry::PushServerEvent(ServerEventEntry entry)
{
	ServerEvents.push_back(std::move(entry));
}

void Registry::PushAppliedServerEvent(std::function<void()> replay)
{
	// World-queue jobs drain at the top of a logic frame, after the previous frame published.
	ServerEvents.push_back({ LastPublishedFrame + 1, std::move(replay), true });
}

void Registry::ReplayServerEventsAt(uint32_t frame)
{
	for (auto& ev : ServerEvents)
	{
		if (ev.Frame != frame || !ev.Replay) continue;
		ev.Replay();
		ev.bApplied = true;
	}
}

void Registry::ApplyPendingServerEventsBefore(uint32_t frame)
{
	for (auto& ev : ServerEvents)
	{
		if (ev.bApplied || ev.Frame >= frame || !ev.Replay) continue;
		ev.Replay();
		ev.bApplied = true;
	}
}

void Registry::PruneServerEvents(uint32_t oldestFrame)
{
	ServerEvents.erase(
		std::remove_if(ServerEvents.begin(), ServerEvents.end(),
			[oldestFrame](const ServerEventEntry& ev)
	{
		return ev.bApplied && ev.Frame < oldestFrame;
	}),
		ServerEvents.end());
}
#endif

// =============================================================================
// Defrag
// =============================================================================

void Registry::TickDefrag(TrinyxJobs::WorldQueueHandle wq)
{
	Defrag.Tick(Archetypes, *this, wq);
}

void Registry::ForceDefragSync()
{
	for (const auto& [key, arch] : Archetypes)
	{
		if (arch->TotalEntityCount < DefragSystem::MinLiveEntities) continue;
		if (arch->AllocatedEntityCount == 0) continue;
		if (arch->InactiveEntitySlots.empty()) continue;

		float holeRatio = 1.0f - static_cast<float>(arch->TotalEntityCount) / static_cast<float>(arch->AllocatedEntityCount);
		if (holeRatio < DefragSystem::HoleThreshold) continue;

		// Loop: MaxMovesPerTick entities are processed per ProcessMoves call.
		// Iterate until the archetype drops below the threshold or has no more holes.
		while (holeRatio >= DefragSystem::HoleThreshold && !arch->InactiveEntitySlots.empty())
		{
			const uint32_t estimatedMoves = arch->AllocatedEntityCount - arch->TotalEntityCount;
			Defrag.ProcessMoves(*this, arch, estimatedMoves, TrinyxJobs::InvalidWorldQueue);

			if (arch->AllocatedEntityCount == 0) break;
			holeRatio = 1.0f - static_cast<float>(arch->TotalEntityCount) / static_cast<float>(arch->AllocatedEntityCount);
		}
	}
}

void Registry::ExecuteDefragMove(Archetype* arch,
	const Archetype::EntitySlot& src,
	const Archetype::EntitySlot& dst)
{
	GlobalEntityHandle gHandle = GlobalEntityRegistry.LookupGlobalHandle(
		static_cast<EntityCacheHandle>(src.CacheIndex));
	EntityRecord* record = GlobalEntityRegistry.Records[gHandle.GetIndex()];
	if (!record || !record->IsValid()) return;

	// Copy all field data: cold fields in-chunk, temporal/volatile across every slab frame.
	arch->MoveEntitySlot(src, dst,
		GetCache(CacheTier::Temporal),
		GetCache(CacheTier::Volatile));

	// Update location fields in the EntityRecord.
	const EntityCacheHandle oldCacheIndex = record->CacheEntityIndex;
	record->ArchIndex                     = dst.ArchIndex;
	record->ChunkIndex                    = dst.ChunkIndex;
	record->LocalIndex                    = dst.LocalIndex;
	record->TargetChunk                   = dst.TargetChunk;
	record->CacheEntityIndex              = static_cast<EntityCacheHandle>(dst.CacheIndex);

	// Remap the CacheToRecord lookup so render/network threads that resolve by
	// cache index find the entity at its new position.
	GlobalEntityRegistry.CacheToRecord.set(dst.CacheIndex, gHandle);
	GlobalEntityRegistry.CacheToRecord.set(src.CacheIndex, GlobalEntityHandle{});

	// Per-chunk live counts: src chunk lost one entity, dst chunk gained one.
	arch->ChunkLiveCounts[src.ChunkIndex]--;
	arch->ChunkLiveCounts[dst.ChunkIndex]++;

	// Notify ConstructViews so they can re-hydrate their field proxy cursors.
	record->OnCacheSlotChange(oldCacheIndex, dst.CacheIndex);
}

void Registry::TrimTailChunks(Archetype* arch)
{
	LOG_ENG_INFO_F("Trimming tail chunks for archetype with %u entities", arch->AllocatedEntityCount);
	while (!arch->Chunks.empty() && arch->ChunkLiveCounts.back() == 0)
	{
		const uint32_t lastChunkIdx       = static_cast<uint32_t>(arch->Chunks.size()) - 1;
		const uint32_t prevAllocCount     = lastChunkIdx * arch->EntitiesPerChunk;
		const uint32_t lastChunkAllocated = arch->AllocatedEntityCount - prevAllocCount;

		// Drop all inactive slot entries that reference this chunk — they're gone.
		arch->InactiveEntitySlots.erase(
			std::remove_if(arch->InactiveEntitySlots.begin(), arch->InactiveEntitySlots.end(),
				[lastChunkIdx](const Archetype::EntitySlot& s)
		{
			return s.ChunkIndex == lastChunkIdx;
		}),
			arch->InactiveEntitySlots.end());

		// Shrink ActiveEntitySlots to match the reduced AllocatedEntityCount.
		arch->ActiveEntitySlots.resize(
			arch->ActiveEntitySlots.size() - lastChunkAllocated);

		// Phase-2 slab defrag: reclaim this chunk's slab allocations before freeing
		// the chunk struct.  NotifyChunkFreed removes entries from ActiveAllocations,
		// decrements CurrentUsed, and stores the freed region in FreedChunkSlabs so
		// the next AllocateChunk for the same archetype can reuse it.
		Chunk* chunk = arch->Chunks.back();
		{
			ComponentCacheBase* vol = GetCache(CacheTier::Volatile);
			vol->NotifyChunkFreed(chunk);
#ifdef TNX_ENABLE_ROLLBACK
			ComponentCacheBase* tmp = GetCache(CacheTier::Temporal);
			if (tmp != vol) tmp->NotifyChunkFreed(chunk);
#endif
		}

		TNX_FREE_N(chunk, arch->DebugName);
#ifdef _MSC_VER
		_aligned_free(chunk);
#else
		free(chunk);
#endif
		arch->Chunks.pop_back();
		arch->ChunkLiveCounts.pop_back();

		arch->AllocatedEntityCount -= lastChunkAllocated;
	}
}

void Registry::InvokeScalarUpdate(SimFloat dt)
{
	TNX_ZONE_C(TNX_COLOR_LOGIC);

	uint32_t hisWrite = 0;
#ifdef TNX_ENABLE_ROLLBACK
	if (!HistorySlab.TryLockFrameForWrite(hisWrite))
	{
		LOG_ENG_WARN_F("Failed to acquire Temporal write lock on frame %u", HistorySlab.GetActiveWriteFrame());
		return;
	}
#endif
	uint32_t volWrite = 0;
	if (!VolatileSlab.TryLockFrameForWrite(volWrite))
	{
		LOG_ENG_WARN_F("Failed to acquire Volatile write lock on frame %u", VolatileSlab.GetActiveWriteFrame());
#ifdef TNX_ENABLE_ROLLBACK
		HistorySlab.UnlockFrameWrite();
#endif
		return;
	}
#ifndef TNX_ENABLE_ROLLBACK
	hisWrite = volWrite; // Without rollback, Temporal fields share the VolatileSlab — match frame indices.
#endif

	TrinyxJobs::JobCounter ScalarUpdateCounter;

	for (auto& [sig, arch] : Archetypes)
	{
		UpdateFunc ScalarUpdate = ReflectionRegistry::Get().EntityGetters[sig.ID].ScalarUpdate;
		if (!ScalarUpdate) continue;

		size_t size = arch->Chunks.size();

		for (size_t chunkIdx = 0; chunkIdx < size; ++chunkIdx)
		{
			TrinyxJobs::Dispatch(
				[ScalarUpdate, arch, chunkIdx, dt, hisWrite, volWrite](uint32_t)
			{
				Chunk* chunk         = arch->Chunks[chunkIdx];
				uint32_t entityCount = arch->GetAllocatedChunkCount(chunkIdx);
				if (entityCount == 0) return;

				void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
				arch->BuildFieldArrayTable(chunk, fieldArrayTable, hisWrite, volWrite);

				ScalarUpdate(dt, fieldArrayTable, fieldArrayTable[0], entityCount);
			},
				&ScalarUpdateCounter, TrinyxJobs::Queue::Logic);
		}
	}

	TrinyxJobs::WaitForCounter(&ScalarUpdateCounter, TrinyxJobs::Queue::Logic);

#ifdef TNX_ENABLE_ROLLBACK
	HistorySlab.UnlockFrameWrite();
#endif
	VolatileSlab.UnlockFrameWrite();
}

void Registry::InvokePrePhys(SimFloat dt)
{
	TNX_ZONE_C(TNX_COLOR_LOGIC);

	uint32_t hisWrite = 0;
#ifdef TNX_ENABLE_ROLLBACK
	if (!HistorySlab.TryLockFrameForWrite(hisWrite))
	{
		LOG_ENG_WARN_F("Failed to acquire Temporal write lock on frame %u", HistorySlab.GetActiveWriteFrame());
		return;
	}
#endif
	uint32_t volWrite = 0;
	if (!VolatileSlab.TryLockFrameForWrite(volWrite))
	{
		LOG_ENG_WARN_F("Failed to acquire Volatile write lock on frame %u", VolatileSlab.GetActiveWriteFrame());
#ifdef TNX_ENABLE_ROLLBACK
		HistorySlab.UnlockFrameWrite();
#endif
		return;
	}
#ifndef TNX_ENABLE_ROLLBACK
	hisWrite = volWrite;
#endif

	TrinyxJobs::JobCounter prePhysCounter;

	for (auto& [sig, arch] : Archetypes)
	{
#ifdef TNX_ENABLE_ROLLBACK
		const EntityMeta& meta = ReflectionRegistry::Get().EntityGetters[sig.ID];
		UpdateFunc prePhys     = (bResimMode && meta.PrePhysResim) ? meta.PrePhysResim : meta.PrePhys;
#else
		UpdateFunc prePhys = ReflectionRegistry::Get().EntityGetters[sig.ID].PrePhys;
#endif
		if (!prePhys) continue;

		// Capture only what fits in 48 bytes: 5 pointers/values = 40 bytes
		size_t chunkCount = arch->Chunks.size();

		for (size_t chunkIdx = 0; chunkIdx < chunkCount; ++chunkIdx)
		{
			TrinyxJobs::Dispatch(
				[prePhys, arch, chunkIdx, dt, hisWrite, volWrite](uint32_t)
			{
				Chunk* chunk         = arch->Chunks[chunkIdx];
				uint32_t entityCount = arch->GetAllocatedChunkCount(chunkIdx);
				if (entityCount == 0) return;

				void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
				arch->BuildFieldArrayTable(chunk, fieldArrayTable, hisWrite, volWrite);

				prePhys(dt, fieldArrayTable, fieldArrayTable[0], entityCount);
			},
				&prePhysCounter, TrinyxJobs::Queue::Logic);
		}
	}

	TrinyxJobs::WaitForCounter(&prePhysCounter, TrinyxJobs::Queue::Logic);

#ifdef TNX_ENABLE_ROLLBACK
	HistorySlab.UnlockFrameWrite();
#endif
	VolatileSlab.UnlockFrameWrite();
}

void Registry::InvokePostPhys(SimFloat dt)
{
	TNX_ZONE_C(TNX_COLOR_LOGIC);

	uint32_t hisWrite = 0;
#ifdef TNX_ENABLE_ROLLBACK
	if (!HistorySlab.TryLockFrameForWrite(hisWrite))
	{
		LOG_ENG_WARN_F("Failed to acquire Temporal write lock on frame %u", HistorySlab.GetActiveWriteFrame());
		return;
	}
#endif
	uint32_t volWrite = 0;
	if (!VolatileSlab.TryLockFrameForWrite(volWrite))
	{
		LOG_ENG_WARN_F("Failed to acquire Volatile write lock on frame %u", VolatileSlab.GetActiveWriteFrame());
#ifdef TNX_ENABLE_ROLLBACK
		HistorySlab.UnlockFrameWrite();
#endif
		return;
	}
#ifndef TNX_ENABLE_ROLLBACK
	hisWrite = volWrite;
#endif

	TrinyxJobs::JobCounter postPhysCounter;

	for (auto& [sig, arch] : Archetypes)
	{
#ifdef TNX_ENABLE_ROLLBACK
		const EntityMeta& meta = ReflectionRegistry::Get().EntityGetters[sig.ID];
		UpdateFunc PostPhys    = (bResimMode && meta.PostPhysResim) ? meta.PostPhysResim : meta.PostPhys;
#else
		UpdateFunc PostPhys = ReflectionRegistry::Get().EntityGetters[sig.ID].PostPhys;
#endif
		if (!PostPhys) continue;

		size_t size = arch->Chunks.size();

		for (size_t chunkIdx = 0; chunkIdx < size; ++chunkIdx)
		{
			TrinyxJobs::Dispatch(
				[PostPhys, arch, chunkIdx, dt, hisWrite, volWrite](uint32_t)
			{
				Chunk* chunk         = arch->Chunks[chunkIdx];
				uint32_t entityCount = arch->GetAllocatedChunkCount(chunkIdx);
				if (entityCount == 0) return;

				void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
				arch->BuildFieldArrayTable(chunk, fieldArrayTable, hisWrite, volWrite);

				PostPhys(dt, fieldArrayTable, fieldArrayTable[0], entityCount);
			},
				&postPhysCounter, TrinyxJobs::Queue::Logic);
		}
	}

	TrinyxJobs::WaitForCounter(&postPhysCounter, TrinyxJobs::Queue::Logic);

#ifdef TNX_ENABLE_ROLLBACK
	HistorySlab.UnlockFrameWrite();
#endif
	VolatileSlab.UnlockFrameWrite();
}
