#pragma once

#include <type_traits>
#include <vector>
#include <memory>
#include <cstdint>
#include "EntityRecord.h"
#include "Types.h"
#include "ConstructRecord.h"
#include "PagedMap.h"

class WorldBase;
class Soul;
class ReplicationSystem;

// ---------------------------------------------------------------------------
// ConstructRegistry — Lifetime-bucketed registry of all live Constructs,
// and the authoritative store for networked Construct lookup.
//
// Constructs are stored in per-ConstructLifetime buckets so that tier-based
// destruction (e.g., destroy all Level + World Constructs on World reset) is
// a direct bucket clear with no per-entry lifetime checks.
//
// Net lookup (Records + NetToRecord) mirrors EntityArchive. ConstructArchive
// has been folded in here — there is no separate archive type.
//
// FlowManager owns the ConstructRegistry so that Session-lifetime Constructs
// survive World destruction. World holds a non-owning pointer passed at init.
// LogicThread calls ProcessDeferredDestructions() at the top of each frame.
//
// Each entry stores optional function pointers for OnWorldTeardown and
// OnWorldInitialized, set at Create time via concept detection on the
// concrete Construct type.
//
// Usage:
//   auto* player = constructs->Create<PlayerConstruct>(world);
//   constructs->Destroy(player->GetConstructID());
// ---------------------------------------------------------------------------
class ConstructRegistry
{
public:
	ConstructRegistry() = default;
	~ConstructRegistry() { DestroyAll(); }

	ConstructRegistry(const ConstructRegistry&)            = delete;
	ConstructRegistry& operator=(const ConstructRegistry&) = delete;

	// --- Callback signatures for world transition hooks ---
	using TeardownFn    = void(*)(void*);               // void OnWorldTeardown()
	using InitializedFn = void(*)(void*, WorldBase*);   // void OnWorldInitialized(WorldBase*)
	using ShutdownFn    = void(*)(void*);               // Construct::Shutdown()
	using ReinitFn      = void(*)(void*, WorldBase*);   // Construct::Initialize(WorldBase*)

	// PreInit is a zero-cost compile-time callable (no std::function overhead).
	// Called after allocation but before Initialize, allowing the caller to
	// set spawn position, soul, etc. before InitializeViews runs.
	// The CRTP PreInit() hook in Construct<T> is still called from Initialize
	// after OwnerWorld is set (and can use GetWorld()). Both paths coexist.
	// Omit the second argument for the no-op path — the branch is compiled away.
	template <typename T, typename PreInit = std::nullptr_t>
	T* Create(WorldBase* InWorld, PreInit&& preInit = nullptr)
	{
		auto typed = std::make_unique<TypedStorage<T>>();
		T* raw     = &typed->Value;

		uint32_t id = NextID++;
		raw->SetConstructID(id);
		if constexpr (!std::is_same_v<std::decay_t<PreInit>, std::nullptr_t>)
			preInit(raw);
		raw->Initialize(InWorld);

		Entry entry;
		entry.ID      = id;
		entry.Ptr     = raw;
		entry.Storage = std::move(typed);

		// Store Shutdown/Initialize for surviving Constructs across World reset
		entry.ShutdownPtr = [](void* p) { static_cast<T*>(p)->Shutdown(); };
		entry.ReinitPtr   = [](void* p, WorldBase* w) { static_cast<T*>(p)->Initialize(w); };

		// Concept-detected world transition callbacks
		if constexpr (requires(T t) { t.OnWorldTeardown(); }) entry.OnTeardown = [](void* p) { static_cast<T*>(p)->OnWorldTeardown(); };

		if constexpr (requires(T t, WorldBase* w) { t.OnWorldInitialized(w); }) entry.OnInitialized = [](void* p, WorldBase* w) { static_cast<T*>(p)->OnWorldInitialized(w); };

		constexpr auto tier = static_cast<uint8_t>(T::Lifetime);
		Buckets[tier].push_back(std::move(entry));
		return raw;
	}

	/// Wire a client-side Construct to the server-assigned ConstructNetHandle.
	/// Called by ReplicationSystem::HandleConstructSpawn — the server's handle is in the payload.
	/// Does NOT allocate a new NetIndex; uses the one already assigned by the server.
	ConstructRef WireNetRef(void* ptr, ConstructNetHandle serverHandle,
							ConstructNetManifest manifest, uint32_t typeHash, uint32_t spawnFrame)
	{
		const uint32_t netIndex = serverHandle.NetIndex;
		const uint8_t ownerID   = serverHandle.NetOwnerID;

		if (netIndex >= NextNetIndex) NextNetIndex = netIndex + 1; // keep client counter ahead of server's

		ConstructRecord rec;
		rec.NetworkID    = serverHandle;
		rec.ConstructPtr = ptr;
		rec.TypeHash     = typeHash;
		rec.PrefabIDRaw  = 0;
		rec.SpawnFrame   = spawnFrame;
		rec.Generation   = 1;
		rec.OwnerID      = ownerID;

		GlobalConstructHandle gHandle(netIndex, 1, manifest.PrefabIndex);
		Records.set(netIndex, rec);
		NetToRecord.set(netIndex, gHandle);

		ConstructRef ref;
		ref.Handle     = serverHandle;
		ref.Generation = 1;
		return ref;
	}

	/// Allocate a network identity for a Construct that has already been created.
	/// Called by ArenaMode (Standalone) and ReplicationSystem::RegisterConstruct (networked).
	/// Allocate a net identity for a Construct (server or standalone).
	/// Returns a valid ConstructRef with a non-zero NetIndex.
	ConstructRef AllocateNetRef(void* ptr, uint8_t ownerID, ConstructNetManifest manifest,
								uint32_t typeHash, int64_t prefabIDRaw, uint32_t spawnFrame = 0)
	{
		const uint32_t netIndex = NextNetIndex++;

		ConstructNetHandle netHandle;
		netHandle.NetOwnerID = ownerID;
		netHandle.NetIndex   = netIndex;

		ConstructRecord rec;
		rec.NetworkID    = netHandle;
		rec.ConstructPtr = ptr;
		rec.TypeHash     = typeHash;
		rec.PrefabIDRaw  = prefabIDRaw;
		rec.SpawnFrame   = spawnFrame;
		rec.Generation   = 1;
		rec.OwnerID      = ownerID;

		GlobalConstructHandle gHandle(netIndex, 1, manifest.PrefabIndex);
		Records.set(netIndex, rec);
		NetToRecord.set(netIndex, gHandle);

		ConstructRef ref;
		ref.Handle     = netHandle;
		ref.Generation = 1;
		return ref;
	}

	/// Create a Construct using the replication path (client-side).
	/// Calls InitializeForReplication(world, handles, count) instead of Initialize(world).
	/// ownerSoul is set on the Construct before InitializeForReplication so ownership
	/// checks (e.g. SetActiveCameraIfOwned) work correctly during initialization.
	template <typename T>
	T* CreateForReplication(WorldBase* InWorld, EntityHandle* viewHandles, uint8_t viewCount, Soul* ownerSoul)
	{
		auto typed = std::make_unique<TypedStorage<T>>();
		T* raw     = &typed->Value;

		uint32_t id = NextID++;
		raw->SetConstructID(id);
		raw->SetOwnerSoul(ownerSoul);
		raw->InitializeForReplication(InWorld, viewHandles, viewCount);

		Entry entry;
		entry.ID      = id;
		entry.Ptr     = raw;
		entry.Storage = std::move(typed);

		entry.ShutdownPtr = [](void* p) { static_cast<T*>(p)->Shutdown(); };
		entry.ReinitPtr   = [](void* p, WorldBase* w) { static_cast<T*>(p)->Initialize(w); };

		if constexpr (requires(T t) { t.OnWorldTeardown(); }) entry.OnTeardown = [](void* p) { static_cast<T*>(p)->OnWorldTeardown(); };
		if constexpr (requires(T t, WorldBase* w) { t.OnWorldInitialized(w); }) entry.OnInitialized = [](void* p, WorldBase* w) { static_cast<T*>(p)->OnWorldInitialized(w); };

		constexpr auto tier = static_cast<uint8_t>(T::Lifetime);
		Buckets[tier].push_back(std::move(entry));
		return raw;
	}

	void Destroy(uint32_t id)
	{
		PendingDestructions.push_back(id);
	}

	/// Process deferred destructions. Called by LogicThread at frame top.
	void ProcessDeferredDestructions()
	{
		if (PendingDestructions.empty()) return;

		for (uint32_t id : PendingDestructions)
		{
			for (auto& bucket : Buckets)
			{
				bool found = false;
				for (size_t i = 0; i < bucket.size(); ++i)
				{
					if (bucket[i].ID == id)
					{
						if (i != bucket.size() - 1) bucket[i] = std::move(bucket.back());
						bucket.pop_back();
						found = true;
						break;
					}
				}
				if (found) break;
			}
		}
		PendingDestructions.clear();
	}

	/// Destroy all Constructs with lifetime below minSurviving.
	/// e.g., DestroyByLifetime(Session) destroys Level + World Constructs.
	/// Destructors run Construct::Shutdown() which deregisters ticks.
	void DestroyByLifetime(ConstructLifetime minSurviving)
	{
		for (uint8_t i = 0; i < static_cast<uint8_t>(minSurviving); ++i)
		{
			Buckets[i].clear(); // unique_ptr destructors call ~TypedStorage → ~Construct → Shutdown
		}
	}

	/// Call OnWorldTeardown on all surviving Constructs (lifetime >= minSurviving).
	/// Then call Shutdown to deregister ticks from the old World's LogicThread.
	void NotifyWorldTeardown(ConstructLifetime minSurviving)
	{
		for (uint8_t i = static_cast<uint8_t>(minSurviving); i < BucketCount; ++i)
		{
			for (auto& entry : Buckets[i])
			{
				if (entry.OnTeardown) entry.OnTeardown(entry.Ptr);
				if (entry.ShutdownPtr) entry.ShutdownPtr(entry.Ptr);
			}
		}
	}

	/// Re-initialize surviving Constructs on a fresh World and call OnWorldInitialized.
	void NotifyWorldInitialized(ConstructLifetime minSurviving, WorldBase* newWorld)
	{
		for (uint8_t i = static_cast<uint8_t>(minSurviving); i < BucketCount; ++i)
		{
			for (auto& entry : Buckets[i])
			{
				if (entry.ReinitPtr) entry.ReinitPtr(entry.Ptr, newWorld);
				if (entry.OnInitialized) entry.OnInitialized(entry.Ptr, newWorld);
			}
		}
	}

	/// Destroy everything.
	void DestroyAll()
	{
		for (auto& bucket : Buckets) bucket.clear();
		PendingDestructions.clear();
	}

	uint32_t GetCount() const
	{
		uint32_t total = 0;
		for (const auto& bucket : Buckets) total += static_cast<uint32_t>(bucket.size());
		return total;
	}

	template <typename Func>
	void ForEach(Func&& fn)
	{
		for (auto& bucket : Buckets) for (auto& entry : bucket) fn(entry.Ptr, entry.ID);
	}

	// --- Net lookup (public read-only) ---

	ConstructRecord GetRecord(ConstructNetHandle handle) const
	{
		const GlobalConstructHandle& gHandle = LookupGlobalHandle(handle);
		return Records.get(gHandle.GetIndex());
	}

	bool IsHandleValid(ConstructNetHandle handle) const
	{
		const GlobalConstructHandle gHandle = LookupGlobalHandle(handle);
		const ConstructRecord* rec          = Records.try_get_ptr(gHandle.GetIndex());
		return rec && gHandle.GetGeneration() == rec->Generation;
	}

	// ConstructRef validation — uses the generation embedded in the ref directly.
	// One fewer map lookup vs IsHandleValid(ConstructNetHandle).
	// Use this for all client→server RPC and net-boundary handle checks.
	bool IsHandleValid(const ConstructRef& ref) const
	{
		const ConstructRecord* rec = Records.try_get_ptr(
			LookupGlobalHandle(ref.Handle).GetIndex());
		return rec && ref.Generation == rec->Generation;
	}

	/// Returns the earliest SpawnFrame across all live Construct records.
	/// Used by LogicThread to clamp rollback targets — we must never roll back to
	/// before the oldest Construct was spawned, as there is no replay event to re-create it.
	uint32_t GetEarliestSpawnFrame() const
	{
		uint32_t earliest = UINT32_MAX;
		for (uint32_t i = 1; i < NextNetIndex; ++i)
		{
			const ConstructRecord* rec = Records.try_get_ptr(i);
			if (rec && rec->IsValid() && rec->SpawnFrame < earliest) earliest = rec->SpawnFrame;
		}
		return earliest;
	}

private:
	// --- Net lookup (private mutable) ---
	friend class ReplicationSystem;

	GlobalConstructHandle LookupGlobalHandle(ConstructNetHandle handle) const
	{
		return NetToRecord.get(handle.GetHandleIndex());
	}

	ConstructRecord* GetRecordPtr(ConstructNetHandle handle)
	{
		if (!IsHandleValid(handle)) return nullptr;
		const GlobalConstructHandle gHandle = LookupGlobalHandle(handle);
		return Records[gHandle.GetIndex()];
	}

	PagedMap<1 << UniqueIndex_Bits, ConstructRecord> Records{};
	PagedMap<1 << UniqueIndex_Bits, GlobalConstructHandle> NetToRecord{};

	struct StorageBase
	{
		virtual ~StorageBase() = default;
	};

	template <typename T>
	struct TypedStorage : StorageBase
	{
		T Value;
	};

	struct Entry
	{
		uint32_t ID = 0;
		void* Ptr   = nullptr;
		std::unique_ptr<StorageBase> Storage;

		// World transition hooks (nullptr if not implemented by the Construct)
		TeardownFn OnTeardown       = nullptr;
		InitializedFn OnInitialized = nullptr;
		ShutdownFn ShutdownPtr      = nullptr;
		ReinitFn ReinitPtr          = nullptr;
	};

	static constexpr uint8_t BucketCount = 4; // Level, World, Session, Persistent
	std::vector<Entry> Buckets[BucketCount];
	std::vector<uint32_t> PendingDestructions;
	uint32_t NextID       = 1;
	uint32_t NextNetIndex = 1; // Monotonic counter; 0 is reserved as "invalid"
};
