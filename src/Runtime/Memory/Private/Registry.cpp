#include "Registry.h"
#include "CacheSlotMeta.h"
#include "JoltPhysics.h"
#include "Profiler.h"
#include <cassert>
#include <immintrin.h>
#include "Archetype.h"

#include "SchemaReflector.h"

Registry::Registry()
	: NextRecordIndex(1) // Start at 1 (0 is reserved for Invalid)
	, NextLocalIndex(1)
	, NextNetIndex(1)
{
	TNX_ZONE_N("Registry::Constructor");
}

Registry::Registry(const EngineConfig* Config)
	: Registry()
{
#ifdef TNX_ENABLE_ROLLBACK
	HistorySlab.Initialize(Config); // Temporal: Config->TemporalFrameCount frames, rollback-capable
#endif
	VolatileSlab.Initialize(Config); // Volatile: 5 frames, no rollback
	InitializeArchetypes();
}

Registry::~Registry()
{
	LOG_INFO_F("Destroying Registry with %zu archetypes", Archetypes.size());

	for (auto& Pair : Archetypes)
	{
		LOG_INFO_F("Deleting archetype with %zu chunks", Pair.second->Chunks.size());
		delete Pair.second;
		LOG_INFO("Archetype deleted successfully");
	}
	Archetypes.clear();
}

// Bridge GHandle → LHandle: allocates a local index from a separate index space,
// wires LocalToRecord so LHandle can resolve back to the record, and stores the
// LHandle on the record itself so the record knows its OOP-facing identity.
EntityHandle Registry::MakeEntityHandle(GlobalEntityHandle GHandle, ClassID classID)
{
	EntityHandle LHandle;
	LHandle.HandleIndex = AllocateLocalIndex();
	LHandle.ClassType   = classID;

	GlobalEntityRegistry.LocalToRecord.set(LHandle.GetHandleIndex(), GHandle);

	EntityRecord* Record = GlobalEntityRegistry.Records[GHandle.GetIndex()];
	if (Record) Record->LHandle = LHandle;

	return LHandle;
}

EntityHandle Registry::CreateByClassID(ClassID classID)
{
	GlobalEntityHandle GHandle;
	CreateInternal(classID, {&GHandle, 1});
	return MakeEntityHandle(GHandle, classID);
}

std::vector<EntityHandle> Registry::CreateByClassID(ClassID classID, size_t count)
{
	std::vector<GlobalEntityHandle> GHandles(count);
	CreateInternal(classID, GHandles);

	std::vector<EntityHandle> handles(count);
	for (size_t i = 0; i < count; ++i) handles[i] = MakeEntityHandle(GHandles[i], classID);
	return handles;
}

void Registry::Recreate(EntityHandle& InHandle)
{
	if (GlobalEntityRegistry.IsHandleValid(InHandle)) Destroy(InHandle);
	InHandle = CreateByClassID(InHandle.GetTypeID());
}

void Registry::RecreateAs(EntityHandle& InHandle, ClassID newClassID)
{
	if (GlobalEntityRegistry.IsHandleValid(InHandle)) Destroy(InHandle);
	InHandle = CreateByClassID(newClassID > 0 ? newClassID : InHandle.GetTypeID());
}

void Registry::RecreateAs(EntityHandle& InHandle, const EntityHandle& asHandle)
{
	if (GlobalEntityRegistry.IsHandleValid(InHandle)) Destroy(InHandle);
	ClassID classID = asHandle.GetTypeID() != InHandle.GetTypeID()
						  ? asHandle.GetTypeID()
						  : InHandle.GetTypeID();
	InHandle = CreateByClassID(classID);
}

Archetype* Registry::GetOrCreateArchetype(const Signature& Sig, const ClassID& ID)
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);
	auto key = Archetype::ArchetypeKey(Sig, ID);

	// Check if archetype already exists
	auto It = Archetypes.find(key);
	if (It)
	{
		return *It;
	}

	// Create new archetype
	auto NewArchetype = new Archetype(Sig, ID);

	// Build component layout from class ID
	std::vector<ComponentMetaEx> Components;
	MetaRegistry& MR = MetaRegistry::Get();

	auto compListIt = MR.ClassToComponentList.find(ID);
	if (compListIt != MR.ClassToComponentList.end())
	{
		ComponentFieldRegistry& CFR = ComponentFieldRegistry::Get();

		for (ComponentTypeID compTypeID : compListIt->second)
		{
			Components.push_back(CFR.GetComponentMeta(compTypeID));
		}
	}

	NewArchetype->BuildLayout(this, Components);

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
			LOG_ERROR_F("Existing entity requested at index: %u", Index);
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

void Registry::FreeGlobalHandle(GlobalEntityHandle GHandle)
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);

	uint32_t Index = GHandle.GetIndex();

	EntityRecord* Record = GlobalEntityRegistry.Records[Index];
	if (!Record || !Record->IsValid())
	{
		LOG_WARN_F("Invalid entity record at index %u", Index);
		return;
	}

	// Defer local/net index recycling — they stay in pending until confirmed safe
	if (Record->LHandle.IsValid()) RequestLocalRecycle(Record->LHandle.GetHandleIndex());
	if (Record->NetworkID.GetHandleIndex() > 0) RequestNetRecycle(Record->NetworkID.GetHandleIndex());

	// Record index goes straight back to the free pool (generation bump prevents stale access)
	FreeRecordIndices.push(Index);

	Record->Arch                = nullptr;
	Record->TargetChunk         = nullptr;
	Record->EntityInfo.ValidBit = false;
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
	for (uint32_t Index : PendingLocalRecycles) FreeLocalIndices.push(Index);
	PendingLocalRecycles.clear();
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
	for (uint32_t Index : PendingNetRecycles) FreeNetIndices.push(Index);
	PendingNetRecycles.clear();
}

// Core creation: allocates GHandles and populates EntityRecords.
// Does NOT allocate local or net handles — that's the caller's responsibility
// via MakeEntityHandle (for local) or a future AssignNetHandle (for network).
void Registry::CreateInternal(ClassID classID, std::span<GlobalEntityHandle> outHandles)
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);

	const size_t count = outHandles.size();
	MetaRegistry& MR   = MetaRegistry::Get();
	Signature sig      = MR.ClassToArchetype[classID];

#ifdef _DEBUG
	if (MR.ClassToArchetype.find(classID) == MR.ClassToArchetype.end())
	{
		const char* name = (classID < 4096) ? MR.EntityGetters[classID].Name : nullptr;
		LOG_ERROR_F("CreateInternal: ClassID %u ('%s') not registered",
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

		// Populate record
		EntityRecord& Record         = GlobalEntityRegistry.Records.findOrAdd(GHandle.GetIndex());
		Record.Arch                  = arch;
		Record.TargetChunk           = Slot.TargetChunk;
		Record.ArchIndex             = Slot.ArchIndex;
		Record.LocalIndex            = Slot.LocalIndex;
		Record.ChunkIndex            = Slot.ChunkIndex;
		Record.EntityInfo.Generation = GHandle.GetGeneration();
		Record.EntityInfo.ValidBit   = true;

		// Cache index → global handle mapping
		GlobalEntityRegistry.CacheToRecord.set(Slot.CacheIndex, GHandle);

		outHandles[i] = GHandle;
	}
}

// Resolves LHandle → GHandle via LocalToRecord, then defers destruction.
// Actual cleanup happens in ProcessDeferredDestructions at end of frame.
void Registry::Destroy(EntityHandle LHandle)
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);

	GlobalEntityHandle GHandle = GlobalEntityRegistry.LookupGlobalHandle(LHandle);
	PendingDestructions.push_back(GHandle);
}

void Registry::DestroyByGlobalHandle(GlobalEntityHandle GHandle)
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);
	PendingDestructions.push_back(GHandle);
}

bool Registry::DestroyRecord(GlobalEntityHandle& GHandle)
{
	EntityRecord* Record = GlobalEntityRegistry.Records[GHandle.GetIndex()];
	if (!Record)
	{
		LOG_ERROR_F("Failed to find record for handle %u during destruction", GHandle.GetIndex());
		return false;
	}
	Archetype* arch = Record->Arch;

	// Remove from archetype
	arch->RemoveEntity(Record->ChunkIndex, Record->LocalIndex, Record->ArchIndex);
	return true;
}

bool Registry::DestroyRecord(EntityRecord& Record)
{
	if (!Record.IsValid())
	{
		LOG_ERROR("Requested record is invalid for destruction");
		return false;
	}
	Archetype* arch = Record.Arch;

	// Remove from archetype
	arch->RemoveEntity(Record.ChunkIndex, Record.LocalIndex, Record.ArchIndex);
	return true;
}

// Processes all deferred destructions queued by Destroy().
// Generation check prevents double-free if the same GHandle was queued twice
// or the slot was already recycled by a prior frame's destruction.
void Registry::ProcessDeferredDestructions()
{
	TNX_ZONE_C(TNX_COLOR_MEMORY);

	if (PendingDestructions.empty()) [[likely]]
		return;

	for (GlobalEntityHandle GHandle : PendingDestructions)
	{
		EntityRecord* Record = GlobalEntityRegistry.Records[GHandle.GetIndex()];

		if (!Record->IsValid()) continue;
		if (Record->GetGeneration() != GHandle.GetGeneration()) continue;

		if (DestroyRecord(*Record))
		{
			FreeGlobalHandle(GHandle);
		}
	}

	PendingDestructions.clear();
}

EntityRecord Registry::GetRecordByCache(EntityCacheHandle CacheHandle) const
{
	GlobalEntityHandle GHandle = GlobalEntityRegistry.LookupGlobalHandle(CacheHandle);
	EntityRecord Record        = GlobalEntityRegistry.Records[GHandle.GetIndex()];
	if (!Record.IsValid() || Record.GetGeneration() != GHandle.GetGeneration()) return EntityRecord{};
	return Record;
}

GlobalEntityHandle Registry::FindEntityByLocation(EntityCacheHandle CacheHandle) const
{
	GlobalEntityHandle GHandle = GlobalEntityRegistry.LookupGlobalHandle(CacheHandle);
	EntityRecord Record        = GlobalEntityRegistry.Records[GHandle.GetIndex()];
	if (!Record.IsValid() || Record.GetGeneration() != GHandle.GetGeneration()) return GlobalEntityHandle();
	return GHandle;
}

void Registry::InitializeArchetypes()
{
	MetaRegistry& MR            = MetaRegistry::Get();
	ComponentFieldRegistry& CFR = ComponentFieldRegistry::Get();

	for (auto& Arch : MR.ClassToArchetype)
	{
		auto key            = Archetype::ArchetypeKey(Arch.second, Arch.first);
		Archetype*& NewArch = Archetypes[key];
		if (!NewArch)
		{
			NewArch = new Archetype(key);
			std::vector<ComponentMetaEx> Components;
			for (auto& CompID : MR.ClassToComponentList[Arch.first])
			{
				Components.push_back(CFR.GetComponentMeta(CompID));
			}
			NewArch->BuildLayout(this, Components, MR.ClassSystemID[Arch.first]);
		}
	}
}

void Registry::PropagateFrame(uint32_t currentFrame)
{
	TNX_ZONE_NC("Propagating Frame", TNX_COLOR_LOGIC)

	TrinyxJobs::JobCounter PropagationCounter;
#ifdef TNX_ENABLE_ROLLBACK
	// Temporal cache: uses circular buffer strategy (defined in ComponentCache<Temporal>)
	HistorySlab.PropagateFrame(PropagationCounter);
#endif

	// Volatile cache: uses triple-buffer strategy with lock-based frame selection
	// (defined in ComponentCache<Volatile>)
	VolatileSlab.PropagateFrame(PropagationCounter);

	TrinyxJobs::WaitForCounter(&PropagationCounter, TrinyxJobs::Queue::Logic);

	// ── Post-propagation dirty bit maintenance ──────────────────────────
	// Bit 29 (DirtiedFrame): cleared unconditionally — it's per-frame only.
	// Bit 30 (Dirty): cleared only if render has caught up (RenderAck >= LastPublishedFrame).
	//
	// After memcpy propagation, the new write frame has inherited all dirty bits.
	// We clear here so this frame starts clean, and FieldProxy sets fresh bits.
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
			const size_t simdCount   = entityCount / 8;
			const size_t remainder   = entityCount % 8;

			const __m256i mask = _mm256_set1_epi32(clearMask);
			auto* ptr          = reinterpret_cast<__m256i*>(flags);
			for (size_t i = 0; i < simdCount; ++i)
			{
				_mm256_storeu_si256(ptr + i, _mm256_and_si256(_mm256_loadu_si256(ptr + i), mask));
			}
			for (size_t i = simdCount * 8; i < simdCount * 8 + remainder; ++i)
			{
				flags[i] &= clearMask;
			}
		}
	}
}

// Hard reset — wipes all entities, handles, free lists, caches, and archetype data.
// Skips the normal deferred destruction path. Used by tests.
void Registry::ResetRegistry()
{
	GlobalEntityRegistry.Records.clear_all();
	GlobalEntityRegistry.NetToRecord.clear_all();
	GlobalEntityRegistry.LocalToRecord.clear_all();
	GlobalEntityRegistry.CacheToRecord.clear_all();

	while (!FreeRecordIndices.empty()) FreeRecordIndices.pop();
	while (!FreeLocalIndices.empty()) FreeLocalIndices.pop();
	while (!FreeNetIndices.empty()) FreeNetIndices.pop();
	PendingLocalRecycles.clear();
	PendingNetRecycles.clear();
	PendingDestructions.clear();

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

	for (auto& arch : Archetypes) arch.second->FreeAllChunks();
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
