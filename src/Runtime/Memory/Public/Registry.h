#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <queue>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Archetype.h"
#include "AssetRegistry.h"
#include "DefragSystem.h"
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

/// @brief Central entity management system.
///
/// Manages three independent handle spaces:
/// - @c GlobalEntityHandle (GHandle) — internal record identity; indexes @c Records[].
/// - @c EntityHandle (LHandle) — OOP / Construct-facing handle; indexes @c LocalToRecord[].
/// - @c EntityNetHandle (NetHandle) — network replication handle; indexes @c NetToRecord[].
///
/// Index 0 is reserved / invalid in all three spaces.
///
/// **Creation flow:**
/// 1. @c CreateInternal(classID, span<GHandle>) — allocates records and archetype slots.
/// 2. @c MakeEntityHandle(GHandle, classID) — allocates a local index, wires @c LocalToRecord.
/// 3. Public API (@c Create<T>, @c CreateByClassID) — wraps the above, returns @c EntityHandle.
///
/// **Destruction flow:**
/// 1. @c Destroy(LHandle) — defers @c GHandle to @c PendingDestructions.
/// 2. @c ProcessDeferredDestructions — removes from archetype, calls @c FreeGlobalHandle.
/// 3. @c FreeGlobalHandle — reclaims record index, queues local / net handle recycling.
/// 4. @c ConfirmLocalRecycles / @c ConfirmNetRecycles — moves pending → free after the safety window.
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
	void ForceDestroyByGlobalHandle(GlobalEntityHandle gHandle);
	void ProcessDeferredDestructions();

	// --- Tombstone API ---
	void ConfirmTombstone(uint32_t recordIndex);
	bool IsTombstoned(uint32_t recordIndex) const;

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

	// --- Defrag (Brain thread) ---
	// Call once per Logic frame (after ProcessDeferredDestructions). On the
	// analysis cadence, scans archetypes and posts WorldQueue move jobs.
	void TickDefrag(TrinyxJobs::WorldQueueHandle wq);

	// Synchronous defrag — bypasses the analysis cadence and runs all compaction
	// inline on the calling thread. No WorldQueue needed. Use for tests only.
	void ForceDefragSync();

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
	// Rule: an event is keyed at the frame its effect is written into. Push on the logic thread
	// (inside SpawnAndWait / PostAndWait lambdas).
	//   - Deferred effects (replicated spawns) are pushed unapplied at their spawn frame and
	//     request a rollback there; the resim writes them into the ring.
	//   - Effects already written this frame (sweeps, activations) use PushAppliedServerEvent,
	//     which keys them at the current sim frame.
	// A rollback that cannot reach an unapplied event's frame (level floor, oldest snapshot,
	// ring depth) applies it at the rewind point instead, so no effect is ever silently lost.
	struct ServerEventEntry
	{
		uint32_t Frame;
		std::function<void()> Replay;
		bool bApplied = false; ///< True once the effect is in the ring at Frame.
	};

	void PushServerEvent(ServerEventEntry entry);
	/// Record an effect already written into the current write frame; keyed at that frame.
	void PushAppliedServerEvent(std::function<void()> replay);
	void ReplayServerEventsAt(uint32_t frame);
	/// At a rollback's rewind point: apply events keyed before @p frame that never reached the ring.
	void ApplyPendingServerEventsBefore(uint32_t frame);
	/// Drops applied events older than @p oldestFrame; unapplied events wait for their rewind.
	void PruneServerEvents(uint32_t oldestFrame);

	// Snapshot all SoA field values for a newly spawned entity and register a server
	// event at 'frame' that restores them during resim. Used by FlowManager::LoadLevel
	// so that rollback across a level-load frame re-hydrates level entity slab slots.
	void PushEntityReinitEvent(GlobalEntityHandle gHandle, uint32_t frame);
#endif

	// Writes all 7 CTransform fields (pos + rot) from a pre-built fieldArrayTable at localIdx
	// and ORs Dirty | DirtiedFrame into fieldArrayTable[0]. Used by all raw-write correction paths
	// to ensure consistent dirty tracking without duplicating the field-layout loop.
	static void WriteEntityTransformFields(void* const* fieldArrayTable, const Archetype* arch,
		uint32_t localIdx,
		SimFloat posX, SimFloat posY, SimFloat posZ,
		SimFloat rotQx, SimFloat rotQy, SimFloat rotQz, SimFloat rotQw);

	// --- Diagnostics ---

	uint32_t GetTotalChunkCount() const;
	uint32_t GetTotalEntityCount() const;
	const auto& GetArchetypes() const { return Archetypes; }

	// Reverse lookup: cache slot → record (read-only copy). Returns invalid record if not found.
	EntityRecord GetRecordByCache(EntityCacheHandle cacheHandle) const;

	// Lookup: EntityHandle → record (read-only copy). Returns invalid record if not found.
	EntityRecord GetRecord(EntityHandle handle) const;

	// Bind/unbind a callback on an entity's OnCacheSlotChange (defrag listener).
	template <typename T, void (T::*MemFn)(uint32_t, uint32_t)>
	void BindOnCacheSlotChange(EntityHandle handle, T* obj)
	{
		GlobalEntityHandle gHandle = GlobalEntityRegistry.LookupGlobalHandle(handle);
		EntityRecord* record       = GlobalEntityRegistry.Records[gHandle.GetIndex()];
		if (record) record->OnCacheSlotChange.template Bind<T, MemFn>(obj);
	}

	template <typename T, void (T::*MemFn)(uint32_t, uint32_t)>
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
	std::atomic<uint32_t> RenderAck{ 0 };
	uint32_t LastPublishedFrame = 0;
	bool RenderHasAcked         = false; // false until render publishes its first ack

	// Track if we should auto-confirm deat entities.
#ifdef TNX_ENABLE_NETWORK
	bool ReplicationActive = true;
#else
	bool ReplicationActive = false;
#endif

private:
	friend class Archetype;
	friend class DefragSystem;
	template <typename, typename, typename>
	friend class LogicThread;
	friend struct RollbackSim;
	friend class ReplicationSystem;
	friend class OwnerNet;
	friend struct EntityBuilder;
	friend struct EntityRecord;
	friend struct EntityArchive;
	friend class TrinyxEngine;
	friend class WorldBase;
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

	// --- Defrag internals (called by DefragSystem) ---

	// Move a single live entity from src to dst within arch, updating EntityRecord,
	// CacheToRecord, ChunkLiveCounts, and firing OnCacheSlotChange.
	// DefragSystem::ProcessMoves is responsible for updating InactiveEntitySlots.
	void ExecuteDefragMove(Archetype* arch,
		const Archetype::EntitySlot& src,
		const Archetype::EntitySlot& dst);

	// Free trailing chunks in arch whose ChunkLiveCounts entry is 0, decrementing
	// AllocatedEntityCount so future iteration scans shrink accordingly.
	void TrimTailChunks(Archetype* arch);

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
	// When bPreserveDirtiedFrame is true (rollback resim), bit 29 is NOT cleared so it
	// accumulates across all resim steps — every entity touched by the resim stays marked.
	void PropagateFrame(uint32_t currentFrame, bool bPreserveDirtiedFrame = false);

#ifdef TNX_ENABLE_ROLLBACK
	// Clear DirtiedFrame (bit 29) on all entities in the current write frame.
	// Called at the start of rollback resim to establish a clean tracking baseline before
	// PropagateFrame calls with bPreserveDirtiedFrame=true accumulate touched entities.
	void ClearDirtiedFrameBits();

	// Set/clear resim mode. When active, InvokePrePhys/InvokePostPhys dispatch to the
	// scalar dirty-filtered variants instead of the wide SIMD variants, and
	// PropagateFrame uses scatter copy instead of full memcpy for the temporal slab.
	void SetResimMode(bool bActive) { bResimMode = bActive; }
#endif

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
	DefragSystem Defrag;

	// Tombstoned record indices (not yet confirmed for destruction)
	std::vector<uint32_t> TombstoneRecordIndices;

	// Confirmed destructions (moved from TombstoneRecordIndices)
	std::vector<GlobalEntityHandle> PendingConfirmedDestructions;

#ifdef TNX_ENABLE_ROLLBACK
	TemporalComponentCache HistorySlab;
	bool bResimMode = false; // set during rollback resim to redirect sweeps to dirty-filtered variants
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
	CreateInternal(classID, { &GHandle, 1 });
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

		view.InitializeInternal();

		AssetRegistry::Get().DrainPendingCheckouts();
	}

	return lHandle;
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
	CreateInternal(classID, { &GHandle, 1 });
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

		view.InitializeInternal();
		fn(view);

		// Warn about mesh-ref fields still at 0 — slot 0 is the invalid sentinel.
		// Material refs are excluded: MaterialID=0 means "no material", which is valid.
		for (const auto& [fkey, fdesc] : record.Arch->ArchetypeFieldLayout)
		{
			if (fdesc.refAssetType != AssetType::Mesh && fdesc.refAssetType != AssetType::Skeleton)
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
	CreateInternal(classID, { &GHandle, 1 });
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
	const ClassID classType      = lHandle.GetTypeID();
	auto& mr                     = ReflectionRegistry::Get();
	// typeID is 1-based; BuildSignature stores components at bit (typeID-1).
	return mr.ClassToArchetype[classType].test(typeID - 1);
}

template <typename... Components>
Signature Registry::BuildSignature()
{
	Signature Sig;
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
		ArchIdx += !!Valid;
	}

	Results.erase(Results.begin() + ArchIdx, Results.end());
	return Results;
}

template <typename... Classes>
std::vector<Archetype*> Registry::ClassQuery()
{
	std::vector<Archetype*> Results(Archetypes.size());
	std::unordered_set<ClassID> ClassIDs{ Classes::StaticClassID()... };
	uint32_t ArchIdx = 0;
	bool Valid       = false;
	for (auto Arch : Archetypes)
	{
		Valid            = ClassIDs.contains(Arch.first.ID);
		Results[ArchIdx] = Arch.second;
		ArchIdx += !!Valid;
	}

	Results.erase(Results.begin() + ArchIdx, Results.end());
	return Results;
}
