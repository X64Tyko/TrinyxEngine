#pragma once
#include <queue>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Archetype.h"
#include "EntityRecord.h"
#include "FlatMap.h"
#include "Schema.h"
#include "Signature.h"
#include "TemporalComponentCache.h"
#include "TrinyxJobs.h"
#include "Types.h"

struct EngineConfig;
class JoltPhysics;

// Registry — Central entity management system.
//
// Manages three independent handle/index spaces:
//   GHandle  (GlobalEntityHandle) — internal record identity, indexes into Records[]
//   LHandle  (EntityHandle)       — OOP/Construct-facing handle, indexes into LocalToRecord[]
//   NetHandle (EntityNetHandle)   — network replication handle, indexes into NetToRecord[]
//
// Index 0 is reserved/invalid in all three spaces.
//
// Creation flow:
//   CreateInternal(classID, span<GHandle>)  — allocates records + archetype slots
//   MakeEntityHandle(GHandle, classID)      — allocates a local index, wires LocalToRecord
//   Public API (Create<T>, CreateByClassID) — wraps the above, returns EntityHandle (LHandle)
//
// Destruction flow:
//   Destroy(LHandle)            — defers GHandle to PendingDestructions
//   ProcessDeferredDestructions — removes from archetype, calls FreeGlobalHandle
//   FreeGlobalHandle            — reclaims record index, requests local/net handle recycling
//   ConfirmLocalRecycles/ConfirmNetRecycles — moves pending → free (call after safety window)
//
class Registry
{
public:
	Registry();
	Registry(const EngineConfig* Config);
	~Registry();

	// --- Entity creation (public API returns LHandles for OOP land) ---

	template <typename T>
	EntityHandle Create();
	template <typename T>
	std::vector<EntityHandle> Create(size_t count);

	// Destroy + recreate at the same ClassID, reusing the handle slot
	void Recreate(EntityHandle& InHandle);

	// Destroy + recreate as a different type
	template <typename T>
	void RecreateAs(EntityHandle& InHandle);
	void RecreateAs(EntityHandle& InHandle, const EntityHandle& asHandle);

	// --- Entity destruction (deferred until ProcessDeferredDestructions) ---

	void Destroy(EntityHandle LHandle);
	void DestroyByGlobalHandle(GlobalEntityHandle GHandle);
	void ProcessDeferredDestructions();

	// --- Component access ---

	template <typename T>
	bool HasComponent(EntityHandle LHandle);

	// --- Queries ---

	template <typename... Components>
	std::vector<Archetype*> ComponentQuery();
	template <typename... Classes>
	std::vector<Archetype*> ClassQuery();

	// --- Lifecycle dispatch (Brain thread) ---

	void InvokeScalarUpdate(double dt);
	void InvokePrePhys(double dt);
	void InvokePostPhys(double dt);

	// --- Slab accessors (used by LogicThread/RenderThread at init time) ---

	ComponentCache<CacheTier::Volatile>* GetVolatileCache() { return &VolatileSlab; }
#ifdef TNX_ENABLE_ROLLBACK
	ComponentCache<CacheTier::Temporal>* GetTemporalCache() { return &HistorySlab; }
#else
	ComponentCache<CacheTier::Volatile>* GetTemporalCache() { return &VolatileSlab; }
#endif

	// --- Diagnostics ---

	uint32_t GetTotalChunkCount() const;
	uint32_t GetTotalEntityCount() const;
	const auto& GetArchetypes() const { return Archetypes; }

	// Reverse lookup: cache slot → record (read-only copy). Returns invalid record if not found.
	EntityRecord GetRecordByCache(EntityCacheHandle CacheHandle) const;

	// Reverse lookup: cache slot → GHandle. Returns default GlobalEntityHandle() if not found.
	GlobalEntityHandle FindEntityByLocation(EntityCacheHandle CacheHandle) const;

private:
	friend class Archetype;
	friend class LogicThread;
	friend struct EntityBuilder;
	friend struct EntityRecord;
	friend struct EntityArchive;
	friend class TrinyxEngine;
	friend class EditorContext;

	void SetPhysics(JoltPhysics* physics) { PhysicsPtr = physics; }
	void ResetRegistry();

	// --- Internal creation pipeline ---
	// CreateInternal is the core: allocates GHandles + populates EntityRecords.
	// CreateByClassID wraps it and stamps LHandles for callers who need EntityHandles.
	// MakeEntityHandle bridges GHandle → LHandle: allocates a local index,
	// wires LocalToRecord[localIdx] → GHandle, and stores LHandle on the record.

	void CreateInternal(ClassID classID, std::span<GlobalEntityHandle> outHandles);
	EntityHandle CreateByClassID(ClassID classID);
	std::vector<EntityHandle> CreateByClassID(ClassID classID, size_t count);
	EntityHandle MakeEntityHandle(GlobalEntityHandle GHandle, ClassID classID);

	void RecreateAs(EntityHandle& InHandle, ClassID newClassID);

	// --- Archetype management ---

	Archetype* GetOrCreateArchetype(const Signature& Sig, const ClassID& ID);
	void InitializeArchetypes();

	// --- Destruction internals ---
	// DestroyRecord removes the entity from its archetype.
	// FreeGlobalHandle reclaims the record index and requests local/net handle recycling.

	bool DestroyRecord(GlobalEntityHandle& GHandle);
	bool DestroyRecord(EntityRecord& Record);

	// --- Slab tier dispatch ---
	// When TNX_ENABLE_ROLLBACK is off, Temporal falls back to the Volatile slab
	// so no HistorySlab member exists and no memory is wasted.

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

	// Propagate SoA data from frame T to T+1
	void PropagateFrame(uint32_t currentFrame);

	// =========================================================================
	// Data members
	// =========================================================================

	EntityArchive GlobalEntityRegistry; // GHandle.GetIndex() → EntityRecord

	// --- Three independent index allocators (0 is invalid in all spaces) ---
	//
	// Record indices map 1:1 with GlobalEntityHandle.Index (the internal identity).
	// Local indices map to EntityHandle.HandleIndex (OOP land references).
	// Net indices map to EntityNetHandle.NetIndex (network replication references).
	//
	// On destruction, local/net indices enter pending recycle lists rather than
	// returning directly to the free pool. This prevents ABA collisions where
	// OOP code or a remote client still holds a stale handle that would alias
	// a newly created entity. Call ConfirmLocalRecycles()/ConfirmNetRecycles()
	// after the safety window (e.g., end of frame for local, network ack for net).

	std::queue<uint32_t> FreeRecordIndices;
	uint32_t NextRecordIndex = 1;

	std::queue<uint32_t> FreeLocalIndices;
	uint32_t NextLocalIndex = 1;
	std::vector<uint32_t> PendingLocalRecycles;

	std::queue<uint32_t> FreeNetIndices;
	uint32_t NextNetIndex = 1;
	std::vector<uint32_t> PendingNetRecycles;

	FlatMap<Archetype::ArchetypeKey, Archetype*> Archetypes;
	std::vector<GlobalEntityHandle> PendingDestructions;

#ifdef TNX_ENABLE_ROLLBACK
	TemporalComponentCache HistorySlab;
#endif
	VolatileComponentCache VolatileSlab;
	JoltPhysics* PhysicsPtr = nullptr;

	// --- Handle allocation/recycling methods ---

	GlobalEntityHandle AllocateGlobalHandle();
	void FreeGlobalHandle(GlobalEntityHandle GHandle);

	uint32_t AllocateLocalIndex();
	uint32_t AllocateNetIndex();

	void RequestLocalRecycle(uint32_t localIndex);
	void RequestNetRecycle(uint32_t netIndex);
	void ConfirmLocalRecycles();
	void ConfirmNetRecycles();

	template <typename... Components>
	Signature BuildSignature();
};


template <typename T>
EntityHandle Registry::Create()
{
	ClassID classID = T::StaticClassID();
	GlobalEntityHandle GHandle;
	CreateInternal(classID, {&GHandle, 1});
	return MakeEntityHandle(GHandle, classID);
}

template <typename T>
void Registry::RecreateAs(EntityHandle& InHandle)
{
	if (GlobalEntityRegistry.IsHandleValid(InHandle)) Destroy(InHandle);

	ClassID TargetType = T::StaticClassID();
	InHandle           = CreateByClassID(TargetType);
}

template <typename T>
std::vector<EntityHandle> Registry::Create(size_t count)
{
	return CreateByClassID(T::StaticClassID(), count);
}

template <typename T>
bool Registry::HasComponent(EntityHandle LHandle)
{
	constexpr ComponentTypeID TypeID = T::StaticTypeID();
	const ClassID Class              = LHandle.GetTypeID();
	MetaRegistry& MR                 = MetaRegistry::Get();
	return (MR.ClassToArchetype[Class] & TypeID) == TypeID;
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
	uint32_t ArchIdx = 0;
	bool Valid       = false;
	Signature Sig    = BuildSignature<Components...>();
	for (auto Arch : Archetypes)
	{
		Valid            = Arch.first.Sig.Contains(Sig);
		Results[ArchIdx] = Arch.second;
		ArchIdx          += !!Valid;
	}

	Results.erase(Results.begin() + ArchIdx, Results.end());
	return Results;
}

template <typename... Classes>
std::vector<Archetype*> Registry::ClassQuery()
{
	std::vector<Archetype*> Results(Archetypes.size());
	std::unordered_set<ClassID> ClassIDs{Classes::StaticClassID()...};
	uint32_t ArchIdx = 0;
	bool Valid       = false;
	for (auto Arch : Archetypes)
	{
		Valid            = ClassIDs.contains(Arch.first.ID);
		Results[ArchIdx] = Arch.second;
		ArchIdx          += !!Valid;
	}

	Results.erase(Results.begin() + ArchIdx, Results.end());
	return Results;
}

inline void Registry::InvokeScalarUpdate(double dt)
{
	TNX_ZONE_C(TNX_COLOR_LOGIC);

	uint32_t hisWrite = 0;
#ifdef TNX_ENABLE_ROLLBACK
	if (!HistorySlab.TryLockFrameForWrite(hisWrite))
	{
		LOG_WARN_F("Failed to acquire Temporal write lock on frame %u", HistorySlab.GetActiveWriteFrame());
		return;
	}
#endif
	uint32_t volWrite = 0;
	if (!VolatileSlab.TryLockFrameForWrite(volWrite))
	{
		LOG_WARN_F("Failed to acquire Volatile write lock on frame %u", VolatileSlab.GetActiveWriteFrame());
#ifdef TNX_ENABLE_ROLLBACK
		HistorySlab.UnlockFrameWrite();
#endif
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

inline void Registry::InvokePrePhys(double dt)
{
	TNX_ZONE_C(TNX_COLOR_LOGIC);

	uint32_t hisWrite = 0;
#ifdef TNX_ENABLE_ROLLBACK
	if (!HistorySlab.TryLockFrameForWrite(hisWrite))
	{
		LOG_WARN_F("Failed to acquire Temporal write lock on frame %u", HistorySlab.GetActiveWriteFrame());
		return;
	}
#endif
	uint32_t volWrite = 0;
	if (!VolatileSlab.TryLockFrameForWrite(volWrite))
	{
		LOG_WARN_F("Failed to acquire Volatile write lock on frame %u", VolatileSlab.GetActiveWriteFrame());
#ifdef TNX_ENABLE_ROLLBACK
		HistorySlab.UnlockFrameWrite();
#endif
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

inline void Registry::InvokePostPhys(double dt)
{
	TNX_ZONE_C(TNX_COLOR_LOGIC);

	uint32_t hisWrite = 0;
#ifdef TNX_ENABLE_ROLLBACK
	if (!HistorySlab.TryLockFrameForWrite(hisWrite))
	{
		LOG_WARN_F("Failed to acquire Temporal write lock on frame %u", HistorySlab.GetActiveWriteFrame());
		return;
	}
#endif
	uint32_t volWrite = 0;
	if (!VolatileSlab.TryLockFrameForWrite(volWrite))
	{
		LOG_WARN_F("Failed to acquire Volatile write lock on frame %u", VolatileSlab.GetActiveWriteFrame());
#ifdef TNX_ENABLE_ROLLBACK
		HistorySlab.UnlockFrameWrite();
#endif
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
