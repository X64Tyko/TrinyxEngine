#pragma once
#include <queue>
#include <unordered_map>
#include <vector>
#include "Archetype.h"
#include "EntityRecord.h"
#include "FieldMeta.h"
#include "Schema.h"
#include "SchemaReflector.h"
#include "Signature.h"
#include "TemporalComponentCache.h"
#include "TemporalFlags.h"
#include "TrinyxJobs.h"
#include "Types.h"

struct EngineConfig;
// Registry - Central entity management system
// Handles entity creation, destruction, and component access
class Registry
{
public:
	Registry();
	Registry(const EngineConfig* Config);
	~Registry();

	// Entity creation, Reflection allows this to be extremely quick
	// Usage: EntityID player = Registry::Get().Create<PlayerController>();
	template <typename T>
	EntityID Create();

	template <typename T>
	std::vector<EntityID> Create(size_t count);

	// Destroy an entity (deferred until end of frame)
	void Destroy(EntityID Id);

	// Get component from entity
	template <typename T>
	T* GetComponent(EntityID Id);

	// Check if entity has component
	template <typename T>
	bool HasComponent(EntityID Id);

	// Apply all pending destructions (called at end of frame)
	void ProcessDeferredDestructions();

	template <typename... Components>
	std::vector<Archetype*> ComponentQuery();

	template <typename... Components>
	std::vector<std::vector<uint64_t>*> ComponentBitsQuery();

	template <typename... Classes>
	std::vector<Archetype*> ClassQuery();


	std::vector<uint64_t>* DirtyBitsFrame(uint32_t inFrame)
	{
		return &EntityDirtyBits[inFrame % EntityDirtyBits.size()];
	}

	// Invoke all lifecycle functions of a specific type
	// currentFrame: frame T (read from T, write to T+1)
	void InvokeScalarUpdate(double dt, uint32_t currentFrame);
	void InvokePrePhys(double dt, uint32_t currentFrame);
	void InvokePostPhys(double dt, uint32_t currentFrame);

	// Slab accessors — used by LogicThread and RenderThread to get cache pointers at init time.
	ComponentCache<CacheTier::Volatile>* GetVolatileCache() { return &VolatileSlab; }
#ifdef TNX_ENABLE_ROLLBACK
	ComponentCache<CacheTier::Temporal>* GetTemporalCache() { return &HistorySlab; }
#else
	ComponentCache<CacheTier::Volatile>* GetTemporalCache() { return &VolatileSlab; }
#endif

	// Memory diagnostics
	uint32_t GetTotalChunkCount() const;
	uint32_t GetTotalEntityCount() const;

	// Resets the registry to default, useful after tests.
	// TODO: this needs to not be public.
	void ResetRegistry();

private:
	friend class Archetype;
	friend class LogicThread;

	// Get or create archetype for a given signature — called from Create<T> and InitializeArchetypes.
	Archetype* GetOrCreateArchetype(const Signature& Sig, const ClassID& ID);

	// Immediately destroy an entity record (swap-and-pop) — called from ProcessDeferredDestructions and ResetRegistry.
	bool DestroyRecord(EntityRecord& Record);

	// Dispatch to the correct slab by tier.
	// When TNX_ENABLE_ROLLBACK is off, Temporal falls back to the Volatile slab so
	// no HistorySlab member exists and no memory is wasted on an empty ring buffer.
	ComponentCacheBase* GetCache(CacheTier tier)
	{
#ifdef TNX_ENABLE_ROLLBACK
		if (tier == CacheTier::Temporal) return &HistorySlab;
#endif
		if (tier == CacheTier::Volatile || tier == CacheTier::Temporal) return &VolatileSlab;
		return nullptr;
	}

	const ComponentCacheBase* GetCache(CacheTier tier) const
	{
#ifdef TNX_ENABLE_ROLLBACK
		if (tier == CacheTier::Temporal) return &HistorySlab;
#endif
		if (tier == CacheTier::Volatile || tier == CacheTier::Temporal) return &VolatileSlab;
		return nullptr;
	}

	// Initialize archetypes with data from MetaRegistry
	void InitializeArchetypes();

	void PropagateFrame(uint32_t currentFrame);

	// Global entity lookup table (indexed by EntityID.GetIndex())
	std::vector<EntityRecord> EntityIndex;

	// Free list for recycled entity indices
	std::queue<uint32_t> FreeIndices;

	// Next entity index to allocate (if free list is empty)
	uint32_t NextEntityIndex = 0;

	std::vector<std::vector<uint64_t>> ComponentAccessBits;
	std::vector<std::vector<uint64_t>> EntityDirtyBits;
	std::vector<uint64_t> EntityActiveBits;

	// Archetype storage (pair<signature, classID> → archetype)
	std::unordered_map<Archetype::ArchetypeKey, Archetype*, ArchetypeKeyHash> Archetypes;

	// Pending destructions (processed at end of frame)
	std::vector<EntityID> PendingDestructions;

#ifdef TNX_ENABLE_ROLLBACK
	ComponentCache<CacheTier::Temporal> HistorySlab; // N frames (Config->TemporalFrameCount), rollback-capable
#endif
	ComponentCache<CacheTier::Volatile> VolatileSlab;   // 5 frames, no rollback

	// Pre-allocated dual array table used by all Invoke* methods.
	// Avoids a large VLA on the stack each call.  Sized by MAX_FIELDS_PER_ARCHETYPE
	// (not the global MAX_FIELD_ARRAYS) because only one archetype is processed at a time.
	// Safe to share because all three Invoke methods run serially on the Brain thread.
	void* FieldBufferTable[MAX_FIELDS_PER_ARCHETYPE]{};

	// Allocate a new EntityID
	EntityID AllocateEntityID(uint16_t TypeID);

	// Free an EntityID (returns index to free list)
	void FreeEntityID(EntityID Id);

	// Helper: Build signature from component list
	template <typename... Components>
	Signature BuildSignature();
};

template <typename T>
EntityID Registry::Create()
{
	return Create<T>(1)[0];
}

template <typename T>
std::vector<EntityID> Registry::Create(size_t count)
{
	// Static local caching - archetype is calculated once per type T
	static Archetype* CachedArchetype = nullptr;
	static bool Initialized           = false;

	if (!Initialized)
	{
		ClassID classID  = T::StaticClassID();
		MetaRegistry& MR = MetaRegistry::Get();

#ifdef _DEBUG // || _WITH_EDITOR
		// Runtime guard: Check if entity type was registered with TNX_REGISTER_ENTITY
		if (MR.ClassToArchetype.find(classID) == MR.ClassToArchetype.end())
		{
			// FATAL: Entity type not registered
			const char* typeName = typeid(T).name();
			LOG_ERROR_F("FATAL: Entity type '%s' not registered! Did you forget TNX_REGISTER_ENTITY(%s)?",
						typeName, typeName);

		// In debug builds, assert. In release, fail gracefully
#ifdef _DEBUG
		assert(false && "Entity type not registered - add TNX_REGISTER_ENTITY macro");
#endif

		// Return invalid entity ID
		return EntityID{};
        }
#endif

		Signature Sig = MR.ClassToArchetype[classID];

		CachedArchetype = GetOrCreateArchetype(Sig, classID);
		Initialized     = true;
	}


	// Allocate slot in archetype
	std::vector<Archetype::EntitySlot> Slots;
	Slots.resize(count);
	EntityIndex.reserve(EntityIndex.size() + count);
	CachedArchetype->PushEntities(Slots, count);

	// Allocate entity ID
	std::vector<EntityID> Entities(count);
	for (size_t i = 0; i < count; ++i)
	{
		EntityID& Id               = Entities[i];
		Archetype::EntitySlot Slot = Slots[i];

		Id = AllocateEntityID(T::StaticClassID());

		// Update EntityIndex
		uint32_t Index = Id.GetIndex();
		if (Index >= EntityIndex.size())
		{
			EntityIndex.resize(Index + 1024);
		}

		EntityRecord& Record = EntityIndex[Index];
		Record.Arch          = CachedArchetype;
		Record.TargetChunk   = Slot.TargetChunk;
		Record.Index         = static_cast<uint16_t>(Slot.LocalIndex);
		Record.ArchetypeIdx  = Slot.GlobalIndex;
		Record.Generation    = Id.GetGeneration();
	}

	return Entities;
}

template <typename T>
T* Registry::GetComponent(EntityID Id)
{
	if (!Id.IsValid()) return nullptr;

	uint32_t Index = Id.GetIndex();
	if (Index >= EntityIndex.size()) return nullptr;

	EntityRecord& Record = EntityIndex[Index];

	// Validate generation (detect use-after-free)
	if (Record.Generation != Id.GetGeneration()) return nullptr;

	if (!Record.IsValid()) return nullptr;

	// TODO: Get ComponentTypeID from reflection (Week 5)
	ComponentTypeID TypeID = T::StaticTypeID();

	// Get component array from archetype
	T* ComponentArray = Record.Arch->GetComponentArray<T>(Record.TargetChunk, TypeID);
	if (!ComponentArray) return nullptr;

	// Return pointer to this entity's component
	return &ComponentArray[Record.Index];
}

template <typename T>
bool Registry::HasComponent(EntityID Id)
{
	return GetComponent<T>(Id) != nullptr;
}

template <typename... Components>
Signature Registry::BuildSignature()
{
	Signature Sig;
	// Fold expression to set all component bits
	((Sig.Set(Components::StaticTypeID() - 1)), ...);
	return Sig;
}

template <typename... Components>
std::vector<Archetype*> Registry::ComponentQuery()
{
	std::vector<Archetype*> Results(Archetypes.size());
	uint8_t ArchIdx = 0;
	bool Valid      = false;
	Signature Sig   = BuildSignature<Components...>();
	for (auto Arch : Archetypes)
	{
		Valid            = Arch.first.Sig.Contains(Sig);
		Results[ArchIdx] = Arch.second;
		ArchIdx          += !!Valid;
	}

	Results.erase(Results.begin() + ArchIdx, Results.end());
	return Results;
}

template <typename... Components>
std::vector<std::vector<uint64_t>*> Registry::ComponentBitsQuery()
{
	std::vector<std::vector<uint64_t>*> Results(sizeof...(Components));
	std::vector<uint32_t> ComponentIDs{Components::StaticTypeID()...};

	int i = 0;
	for (auto& ID : ComponentIDs)
	{
		Results[i++] = &ComponentAccessBits[ID];
	}
	return Results;
}

template <typename... Classes>
std::vector<Archetype*> Registry::ClassQuery()
{
	std::vector<Archetype*> Results(Archetypes.size());
	std::unordered_set<ClassID> ClassIDs{Classes::StaticClassID()...};
	uint8_t ArchIdx = 0;
	bool Valid      = false;
	for (auto Arch : Archetypes)
	{
		Valid            = ClassIDs.contains(Arch.first.ID);
		Results[ArchIdx] = Arch.second;
		ArchIdx          += !!Valid;
	}

	Results.erase(Results.begin() + ArchIdx, Results.end());
	return Results;
}

inline void Registry::InvokeScalarUpdate(double dt, uint32_t currentFrame)
{
	TNX_ZONE_C(TNX_COLOR_LOGIC);

#ifdef TNX_ENABLE_ROLLBACK
	uint32_t histWrite = HistorySlab.GetNextFrame(currentFrame);
	if (!HistorySlab.TryLockFrameForWrite(histWrite))
	{
		LOG_ERROR_F("Failed to acquire Temporal write lock on frame %u", histWrite);
		return;
	}
#endif
	uint32_t volWrite = VolatileSlab.GetNextFrame(currentFrame);
	if (!VolatileSlab.TryLockFrameForWrite(volWrite))
	{
		LOG_ERROR_F("Failed to acquire Volatile write lock on frame %u", volWrite);
#ifdef TNX_ENABLE_ROLLBACK
		HistorySlab.UnlockFrameWrite(histWrite);
#endif
		return;
	}
#ifdef TNX_ENABLE_ROLLBACK
	if (!HistorySlab.VerifyFrameReadable(currentFrame) || !VolatileSlab.VerifyFrameReadable(currentFrame))
#else
	if (!VolatileSlab.VerifyFrameReadable(currentFrame))
#endif
	{
		LOG_ERROR_F("Frame %u is not readable on one or both slabs", currentFrame);
#ifdef TNX_ENABLE_ROLLBACK
		HistorySlab.UnlockFrameWrite(histWrite);
#endif
		VolatileSlab.UnlockFrameWrite(volWrite);
		return;
	}

	TrinyxJobs::JobCounter ScalarUpdateCounter;

	for (auto& [sig, arch] : Archetypes)
	{
		UpdateFunc ScalarUpdate = MetaRegistry::Get().EntityGetters[sig.ID].ScalarUpdate;
		if (!ScalarUpdate) continue;

		size_t size = arch->Chunks.size();

		for (size_t chunkIdx = 0; chunkIdx < size; ++chunkIdx)
		{
			TrinyxJobs::Dispatch(
				[this, ScalarUpdate, arch, chunkIdx, dt, currentFrame](uint32_t)
				{
					Chunk* chunk         = arch->Chunks[chunkIdx];
					uint32_t entityCount = arch->GetChunkCount(chunkIdx);
					if (entityCount == 0) return;

					void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
					arch->BuildFieldArrayTable(chunk, fieldArrayTable, currentFrame);
					uint8_t* chunkDirtyBits = reinterpret_cast<uint8_t*>(
							this->DirtyBitsFrame(currentFrame)->data())
						+ (chunk->Header.GlobalIndexStart / 8);
					ScalarUpdate(dt, fieldArrayTable, chunkDirtyBits, entityCount);
				},
				&ScalarUpdateCounter, TrinyxJobs::Queue::Physics);
		}
	}

	TrinyxJobs::LogicWaitForCounter(&ScalarUpdateCounter);

#ifdef TNX_ENABLE_ROLLBACK
	HistorySlab.UnlockFrameWrite(histWrite);
#endif
	VolatileSlab.UnlockFrameWrite(volWrite);
}

inline void Registry::InvokePrePhys(double dt, uint32_t currentFrame)
{
	TNX_ZONE_C(TNX_COLOR_LOGIC);

#ifdef TNX_ENABLE_ROLLBACK
	uint32_t histWrite = HistorySlab.GetNextFrame(currentFrame);
	if (!HistorySlab.TryLockFrameForWrite(histWrite))
	{
		LOG_ERROR_F("Failed to acquire Temporal write lock on frame %u", histWrite);
		return;
	}
#endif
	uint32_t volWrite = VolatileSlab.GetNextFrame(currentFrame);
	if (!VolatileSlab.TryLockFrameForWrite(volWrite))
	{
		LOG_ERROR_F("Failed to acquire Volatile write lock on frame %u", volWrite);
#ifdef TNX_ENABLE_ROLLBACK
		HistorySlab.UnlockFrameWrite(histWrite);
#endif
		return;
	}
#ifdef TNX_ENABLE_ROLLBACK
	if (!HistorySlab.VerifyFrameReadable(currentFrame) || !VolatileSlab.VerifyFrameReadable(currentFrame))
#else
	if (!VolatileSlab.VerifyFrameReadable(currentFrame))
#endif
	{
		LOG_ERROR_F("Frame %u is not readable on one or both slabs", currentFrame);
#ifdef TNX_ENABLE_ROLLBACK
		HistorySlab.UnlockFrameWrite(histWrite);
#endif
		VolatileSlab.UnlockFrameWrite(volWrite);
		return;
	}

	TrinyxJobs::JobCounter prePhysCounter;

	for (auto& [sig, arch] : Archetypes)
	{
		UpdateFunc prePhys = MetaRegistry::Get().EntityGetters[sig.ID].PrePhys;
		if (!prePhys) continue;

		// Capture only what fits in 48 bytes: 5 pointers/values = 40 bytes
		size_t chunkCount = arch->Chunks.size();

		for (size_t chunkIdx = 0; chunkIdx < chunkCount; ++chunkIdx)
		{
			TrinyxJobs::Dispatch(
				[this, prePhys, arch, chunkIdx, dt, currentFrame](uint32_t)
				{
					Chunk* chunk         = arch->Chunks[chunkIdx];
					uint32_t entityCount = arch->GetChunkCount(chunkIdx);
					if (entityCount == 0) return;

					void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
					arch->BuildFieldArrayTable(chunk, fieldArrayTable, currentFrame);
					uint8_t* chunkDirtyBits = reinterpret_cast<uint8_t*>(
							this->DirtyBitsFrame(currentFrame)->data())
						+ (chunk->Header.GlobalIndexStart / 8);
					prePhys(dt, fieldArrayTable, chunkDirtyBits, entityCount);
				},
				&prePhysCounter, TrinyxJobs::Queue::Physics);
		}
	}

	TrinyxJobs::LogicWaitForCounter(&prePhysCounter);

#ifdef TNX_ENABLE_ROLLBACK
	HistorySlab.UnlockFrameWrite(histWrite);
#endif
	VolatileSlab.UnlockFrameWrite(volWrite);
}

inline void Registry::InvokePostPhys(double dt, uint32_t currentFrame)
{
	TNX_ZONE_C(TNX_COLOR_LOGIC);

#ifdef TNX_ENABLE_ROLLBACK
	uint32_t histWrite = HistorySlab.GetNextFrame(currentFrame);
	if (!HistorySlab.TryLockFrameForWrite(histWrite))
	{
		LOG_ERROR_F("Failed to acquire Temporal write lock on frame %u", histWrite);
		return;
	}
#endif
	uint32_t volWrite = VolatileSlab.GetNextFrame(currentFrame);
	if (!VolatileSlab.TryLockFrameForWrite(volWrite))
	{
		LOG_ERROR_F("Failed to acquire Volatile write lock on frame %u", volWrite);
#ifdef TNX_ENABLE_ROLLBACK
		HistorySlab.UnlockFrameWrite(histWrite);
#endif
		return;
	}
#ifdef TNX_ENABLE_ROLLBACK
	if (!HistorySlab.VerifyFrameReadable(currentFrame) || !VolatileSlab.VerifyFrameReadable(currentFrame))
#else
	if (!VolatileSlab.VerifyFrameReadable(currentFrame))
#endif
	{
		LOG_ERROR_F("Frame %u is not readable on one or both slabs", currentFrame);
#ifdef TNX_ENABLE_ROLLBACK
		HistorySlab.UnlockFrameWrite(histWrite);
#endif
		VolatileSlab.UnlockFrameWrite(volWrite);
		return;
	}
	
	TrinyxJobs::JobCounter postPhysCounter;

	for (auto& [sig, arch] : Archetypes)
	{
		UpdateFunc PostPhys = MetaRegistry::Get().EntityGetters[sig.ID].PostPhys;
		if (!PostPhys) continue;

		size_t size = arch->Chunks.size();

		for (size_t chunkIdx = 0; chunkIdx < size; ++chunkIdx)
		{
			TrinyxJobs::Dispatch(
				[this, PostPhys, arch, chunkIdx, dt, currentFrame](uint32_t)
				{
					Chunk* chunk         = arch->Chunks[chunkIdx];
					uint32_t entityCount = arch->GetChunkCount(chunkIdx);
					if (entityCount == 0) return;

					void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
					arch->BuildFieldArrayTable(chunk, fieldArrayTable, currentFrame);
					uint8_t* chunkDirtyBits = reinterpret_cast<uint8_t*>(
							this->DirtyBitsFrame(currentFrame)->data())
						+ (chunk->Header.GlobalIndexStart / 8);
					PostPhys(dt, fieldArrayTable, chunkDirtyBits, entityCount);
				},
				&postPhysCounter, TrinyxJobs::Queue::Physics);
		}
	}

	TrinyxJobs::LogicWaitForCounter(&postPhysCounter);

#ifdef TNX_ENABLE_ROLLBACK
	HistorySlab.UnlockFrameWrite(histWrite);
#endif
	VolatileSlab.UnlockFrameWrite(volWrite);
}