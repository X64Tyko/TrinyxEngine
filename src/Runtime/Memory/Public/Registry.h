#pragma once
#include <atomic>
#include <functional>
#include <queue>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Archetype.h"
#include "AssetRegistry.h"
#include "EntityRecord.h"
#include "FlatMap.h"
#include "ReflectionRegistry.h"
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
	Registry(const EngineConfig* config);
	~Registry();

	// --- Entity creation (public API returns LHandles for OOP land) ---

	template <typename T>
	EntityHandle Create();
	template <typename T, std::invocable<T&> Fn>
	EntityHandle Create(Fn&& fn);
	template <typename T>
	std::vector<EntityHandle> Create(size_t count);

	// Type-erased init-lambda create — used by EntityBuilder for runtime ClassID spawning.
	// fn receives (record, fieldArrayTable) immediately after entity allocation; pending
	// asset checkouts registered during fn are drained automatically before returning.
	template <std::invocable<EntityRecord&, void**> Fn>
	EntityHandle CreateByClassID(ClassID classID, Fn&& fn);

	// Destroy + recreate at the same ClassID, reusing the handle slot
	void Recreate(EntityHandle& inHandle);

	// Destroy + recreate as a different type
	template <typename T>
	void RecreateAs(EntityHandle& inHandle);
	void RecreateAs(EntityHandle& inHandle, const EntityHandle& asHandle);

	// --- Entity destruction (deferred until ProcessDeferredDestructions) ---

	void Destroy(EntityHandle lHandle);
	void DestroyByGlobalHandle(GlobalEntityHandle gHandle);
	void ProcessDeferredDestructions();

	// --- Component access ---

	template <typename T>
	bool HasComponent(EntityHandle lHandle);

	// --- Queries ---

	template <typename... Components>
	std::vector<Archetype*> ComponentQuery();
	template <typename... Classes>
	std::vector<Archetype*> ClassQuery();

	// --- Lifecycle dispatch (Brain thread) ---

	void InvokeScalarUpdate(SimFloat dt);
	void InvokePrePhys(SimFloat dt);
	void InvokePostPhys(SimFloat dt);

	// --- Slab accessors (used by LogicThread/RenderThread at init time) ---

	ComponentCache<CacheTier::Volatile>* GetVolatileCache() { return &VolatileSlab; }
#ifdef TNX_ENABLE_ROLLBACK
	ComponentCache<CacheTier::Temporal>* GetTemporalCache() { return &HistorySlab; }
#else
	ComponentCache<CacheTier::Volatile>* GetTemporalCache() { return &VolatileSlab; }
#endif

	// --- Replication helpers ---

	// Promote all Alive-but-not-Active entities to Active in the temporal cache.
	// Must be called on the Logic thread. Returns the count of entities promoted.
	int SweepAliveFlagsToActive();

#ifdef TNX_ENABLE_ROLLBACK
	// During rollback resim: re-read this entity's current write-frame position and compare
	// against the server-authoritative value in correction. If still divergent, overwrite
	// all CTransform fields (pos + rot) and return true. Returns false if converged.
	bool CheckAndCorrectEntityTransform(const EntityTransformCorrection& correction);

	// Server-driven discrete events (spawns, sweeps) that must be replayed during rollback
	// resim so the corrected timeline stays deterministically consistent with the server.
	//
	// Push once on the logic thread when the event first executes (inside SpawnAndWait /
	// PostAndWait lambdas). During resim, ReplayServerEventsAt replays all events whose
	// frame matches the current resim frame. PruneServerEvents drops events that have aged
	// out of the temporal ring — they can never be targeted by a rollback.
	struct ServerEventEntry
	{
		uint32_t Frame;
		std::function<void()> Replay;
	};

	void PushServerEvent(ServerEventEntry entry);
	void ReplayServerEventsAt(uint32_t frame);
	void PruneServerEvents(uint32_t oldestFrame);

	// Snapshot all SoA field values for a newly spawned entity and register a server
	// event at 'frame' that restores them during resim. Used by FlowManager::LoadLevel
	// so that rollback across a level-load frame re-hydrates level entity slab slots.
	void PushEntityReinitEvent(GlobalEntityHandle gHandle, uint32_t frame);
#endif

	// --- Diagnostics ---

	uint32_t GetTotalChunkCount() const;
	uint32_t GetTotalEntityCount() const;
	const auto& GetArchetypes() const { return Archetypes; }

	// Reverse lookup: cache slot → record (read-only copy). Returns invalid record if not found.
	EntityRecord GetRecordByCache(EntityCacheHandle cacheHandle) const;

	// Lookup: EntityHandle → record (read-only copy). Returns invalid record if not found.
	EntityRecord GetRecord(EntityHandle handle) const;

	// Bind/unbind a callback on an entity's OnCacheSlotChange (defrag listener).
	template <typename T, void(T::*MemFn)(uint32_t, uint32_t)>
	void BindOnCacheSlotChange(EntityHandle handle, T* obj)
	{
		GlobalEntityHandle gHandle = GlobalEntityRegistry.LookupGlobalHandle(handle);
		EntityRecord* record       = GlobalEntityRegistry.Records[gHandle.GetIndex()];
		if (record) record->OnCacheSlotChange.template Bind<T, MemFn>(obj);
	}

	template <typename T, void(T::*MemFn)(uint32_t, uint32_t)>
	void UnbindOnCacheSlotChange(EntityHandle handle, T* obj)
	{
		GlobalEntityHandle gHandle = GlobalEntityRegistry.LookupGlobalHandle(handle);
		EntityRecord* record       = GlobalEntityRegistry.Records[gHandle.GetIndex()];
		if (record) record->OnCacheSlotChange.template Unbind<T, MemFn>(obj);
	}

	// Reverse lookup: cache slot → GHandle. Returns default GlobalEntityHandle() if not found.
	GlobalEntityHandle FindEntityByLocation(EntityCacheHandle cacheHandle) const;

	// Render → Logic handshake: render publishes the logic frame number it just consumed.
	// Logic reads this to decide whether to clear accumulated dirty bits (bit 30).
	std::atomic<uint32_t> RenderAck{0};
	uint32_t LastPublishedFrame = 0;
	bool RenderHasAcked         = false; // false until render publishes its first ack

private:
	friend class Archetype;
	friend class LogicThread;
	friend class ReplicationSystem;
	friend class OwnerNet;
	friend struct EntityBuilder;
	friend struct EntityRecord;
	friend struct EntityArchive;
	friend class TrinyxEngine;
	friend class World;
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
	EntityHandle MakeEntityHandle(GlobalEntityHandle gHandle, ClassID classID);

	void RecreateAs(EntityHandle& inHandle, ClassID newClassID);

	// --- Archetype management ---

	Archetype* GetOrCreateArchetype(const Signature& sig, const ClassID& id);
	void InitializeArchetypes();

	// --- Destruction internals ---
	// DestroyRecord removes the entity from its archetype.
	// FreeGlobalHandle reclaims the record index and requests local/net handle recycling.

	bool DestroyRecord(GlobalEntityHandle& gHandle);
	bool DestroyRecord(EntityRecord& record);

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

	// Propagate SoA data from frame T to T+1.
	// After propagation: clears DirtiedFrame (bit 29) unconditionally,
	// clears Dirty (bit 30) if render has acknowledged the last published frame.
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

#ifdef TNX_ENABLE_ROLLBACK
	// All calls to PushServerEvent execute on the logic thread (inside PostAndWait /
	// SpawnAndWait lambdas). No cross-thread access — no lock needed.
	std::vector<ServerEventEntry> ServerEvents;
#endif

	// --- Handle allocation/recycling methods ---

	GlobalEntityHandle AllocateGlobalHandle();
	void FreeGlobalHandle(GlobalEntityHandle gHandle);

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

// Create<T>(fn) — entity creation with an init lambda.
//
// Hydrates a transient Scalar view bound to the entity's live slab slot, then calls
// fn(view). Component assignment operators (e.g., CMeshRef::SetMesh, CMeshRef::operator=)
// push to a thread-local pending checkout list during the lambda. After fn returns, all
// pending checkouts are drained: OnLoaded/OnEvicted callbacks are bound with the field's
// stable slab pointer as the context. The view is discarded after initialization.
//
// If any asset-ref fields remain at slot 0 after the lambda, a warning is logged per field.
template <typename T, std::invocable<T&> Fn>
EntityHandle Registry::Create(Fn&& fn)
{
	ClassID classID = T::StaticClassID();
	GlobalEntityHandle GHandle;
	CreateInternal(classID, {&GHandle, 1});
	EntityHandle lHandle = MakeEntityHandle(GHandle, classID);

	EntityRecord record = GetRecord(lHandle);
	if (record.IsValid())
	{
		uint32_t temporalWrite = GetTemporalCache()->GetActiveWriteFrame();
		uint32_t volatileWrite = GetVolatileCache()->GetActiveWriteFrame();

		void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
		record.Arch->BuildFieldArrayTable(record.TargetChunk, fieldArrayTable, temporalWrite, volatileWrite);

		T view;
		view.Hydrate(fieldArrayTable, fieldArrayTable[0], record.LocalIndex);

		fn(view);

		// Warn about mesh-ref fields still at 0 — slot 0 is the invalid sentinel.
		// Material refs are excluded: MaterialID=0 means "no material", which is valid.
		for (const auto& [fkey, fdesc] : record.Arch->ArchetypeFieldLayout)
		{
			if (fdesc.refAssetType != AssetType::StaticMesh &&
				fdesc.refAssetType != AssetType::SkeletalMesh)
				continue;
			auto* arr    = static_cast<uint32_t*>(fieldArrayTable[fdesc.fieldSlotIndex]);
			uint32_t val = arr[record.LocalIndex];
			if (val == 0)
				LOG_ENG_WARN("Registry::Create - MeshID not set (slot 0 is invalid; use SetMesh in your init lambda)");
		}

		AssetRegistry::Get().DrainPendingCheckouts();
	}

	return lHandle;
}

// CreateByClassID(classID, fn) — type-erased init-lambda create.
//
// Allocates an entity by runtime ClassID, builds the field array table, and invokes
// fn(record, fieldArrayTable). The caller (e.g. EntityBuilder) can write raw field data
// and call RegisterPendingCheckout for asset-ref fields. Pending checkouts are drained
// after fn returns. Mesh-ref fields still at 0 emit a warning (slot 0 is the invalid sentinel).
template <std::invocable<EntityRecord&, void**> Fn>
EntityHandle Registry::CreateByClassID(ClassID classID, Fn&& fn)
{
	GlobalEntityHandle GHandle;
	CreateInternal(classID, {&GHandle, 1});
	EntityHandle lHandle = MakeEntityHandle(GHandle, classID);

	EntityRecord record = GetRecord(lHandle);
	if (record.IsValid())
	{
		void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
		record.Arch->BuildFieldArrayTable(record.TargetChunk, fieldArrayTable,
										  GetTemporalCache()->GetActiveWriteFrame(),
										  GetVolatileCache()->GetActiveWriteFrame());

		fn(record, fieldArrayTable);

		AssetRegistry::Get().DrainPendingCheckouts();
	}

	return lHandle;
}

template <typename T>
void Registry::RecreateAs(EntityHandle& inHandle)
{
	if (GlobalEntityRegistry.IsHandleValid(inHandle)) Destroy(inHandle);

	ClassID targetType = T::StaticClassID();
	inHandle           = CreateByClassID(targetType);
}

template <typename T>
std::vector<EntityHandle> Registry::Create(size_t count)
{
	return CreateByClassID(T::StaticClassID(), count);
}

template <typename T>
bool Registry::HasComponent(EntityHandle lHandle)
{
	const ComponentTypeID typeID = T::StaticTypeID(); // StaticTypeID() is runtime-const, not constexpr
	const ClassID classType          = lHandle.GetTypeID();
	auto& mr                         = ReflectionRegistry::Get();
	// typeID is 1-based; BuildSignature stores components at bit (typeID-1).
	return mr.ClassToArchetype[classType].test(typeID - 1);
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

inline void Registry::InvokeScalarUpdate(SimFloat dt)
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

inline void Registry::InvokePrePhys(SimFloat dt)
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

	TrinyxJobs::JobCounter prePhysCounter;

	for (auto& [sig, arch] : Archetypes)
	{
		UpdateFunc prePhys = ReflectionRegistry::Get().EntityGetters[sig.ID].PrePhys;
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

inline void Registry::InvokePostPhys(SimFloat dt)
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

	TrinyxJobs::JobCounter postPhysCounter;

	for (auto& [sig, arch] : Archetypes)
	{
		UpdateFunc PostPhys = ReflectionRegistry::Get().EntityGetters[sig.ID].PostPhys;
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
