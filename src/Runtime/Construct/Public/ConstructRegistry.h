#pragma once

#include <cstring>
#include <type_traits>
#include <typeinfo>
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
	using TeardownFn    = void (*)(void*);             // void OnWorldTeardown()
	using InitializedFn = void (*)(void*, WorldBase*); // void OnWorldInitialized(WorldBase*)
	using ShutdownFn    = void (*)(void*);             // Construct::Shutdown()
	using ReinitFn      = void (*)(void*, WorldBase*); // Construct::Initialize(WorldBase*)

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

		const uint32_t id = NextID++;
		raw->SetConstructID(id);
		if constexpr (!std::is_same_v<std::decay_t<PreInit>, std::nullptr_t>)
			preInit(raw);
		raw->Initialize(InWorld);

		Entry entry                = MakeEntry<T>(id, raw, std::move(typed));
		entry.CollectViewHandlesFn = [](void* p, std::vector<EntityHandle>& out)
		{
			static_cast<T*>(p)->CollectViewHandles(out);
		};
		AddEntry(static_cast<uint8_t>(T::Lifetime), std::move(entry));
		return raw;
	}

	/// Wire a client-side Construct to the server-assigned ConstructNetHandle.
	/// Called by ReplicationSystem::HandleConstructSpawn — the server's handle is in the payload.
	/// Does NOT allocate a new NetIndex; uses the one already assigned by the server.
	ConstructRef WireNetRef(void* ptr, ConstructNetHandle serverHandle,
		ConstructNetManifest manifest, uint32_t typeHash, uint32_t spawnFrame);

	/// Allocate a network identity for a Construct that has already been created.
	/// Called by ArenaMode (Standalone) and ReplicationSystem::RegisterConstruct (networked).
	/// Allocate a net identity for a Construct (server or standalone).
	/// Returns a valid ConstructRef with a non-zero NetIndex.
	ConstructRef AllocateNetRef(void* ptr, uint8_t ownerID, ConstructNetManifest manifest,
		uint32_t typeHash, int64_t prefabIDRaw, uint32_t spawnFrame = 0);

	/// Create a Construct using the replication path (client-side).
	/// Calls InitializeForReplication(world, handles, count) instead of Initialize(world).
	/// ownerSoul is set on the Construct before InitializeForReplication so ownership
	/// checks (e.g. SetActiveCameraIfOwned) work correctly during initialization.
	template <typename T>
	T* CreateForReplication(WorldBase* InWorld, EntityHandle* viewHandles, uint8_t viewCount, Soul* ownerSoul)
	{
		auto typed = std::make_unique<TypedStorage<T>>();
		T* raw     = &typed->Value;

		const uint32_t id = NextID++;
		raw->SetConstructID(id);
		raw->SetOwnerSoul(ownerSoul);
		raw->InitializeForReplication(InWorld, viewHandles, viewCount);

		AddEntry(static_cast<uint8_t>(T::Lifetime), MakeEntry<T>(id, raw, std::move(typed)));
		return raw;
	}

	void Destroy(uint32_t id)
	{
		PendingDestructions.push_back(id);
	}

	/// Destroy a Construct by its network handle. Searches Entry buckets for the
	/// matching pointer (same pattern as SetNetDestroyHook). Safe to call on Logic thread.
	void DestroyByNetHandle(ConstructNetHandle handle);

	/// Called by ReplicationSystem after RegisterConstruct to auto-deregister on destruction.
	void SetNetDestroyHook(void* ptr, ConstructNetHandle handle,
		void (*fn)(void*, ConstructNetHandle), void* ctx);

	/// Clear all net destroy hooks — called by ReplicationSystem on destruction
	/// to prevent stale callbacks after the system is torn down.
	void ClearNetDestroyHooks();

	/// Process deferred destructions. Called by LogicThread at frame top.
	void ProcessDeferredDestructions();

	/// Destroy all Constructs with lifetime below minSurviving.
	/// e.g., DestroyByLifetime(Session) destroys Level + World Constructs.
	/// Destructors run Construct::Shutdown() which deregisters ticks.
	void DestroyByLifetime(ConstructLifetime minSurviving);

	/// Call OnWorldTeardown on all surviving Constructs (lifetime >= minSurviving).
	/// Then call Shutdown to deregister ticks from the old World's LogicThread.
	void NotifyWorldTeardown(ConstructLifetime minSurviving);

	/// Re-initialize surviving Constructs on a fresh World and call OnWorldInitialized.
	void NotifyWorldInitialized(ConstructLifetime minSurviving, WorldBase* newWorld);

	/// Destroy everything.
	void DestroyAll();

	uint32_t GetCount() const;

	template <typename Func>
	void ForEach(Func&& fn)
	{
		for (auto& bucket : Buckets)
			for (auto& entry : bucket)
				fn(entry.Ptr, entry.ID);
	}

	/// Variant that also passes the raw typeid name for editor display.
	template <typename Func>
	void ForEachWithMeta(Func&& fn)
	{
		for (auto& bucket : Buckets)
			for (auto& entry : bucket)
				fn(entry.Ptr, entry.ID, entry.TypeName);
	}

	// --- Net lookup (public read-only) ---

	ConstructRecord GetRecord(ConstructNetHandle handle) const;

	bool IsHandleValid(ConstructNetHandle handle) const;

	// ConstructRef validation — uses the generation embedded in the ref directly.
	// One fewer map lookup vs IsHandleValid(ConstructNetHandle).
	// Use this for all client→server RPC and net-boundary handle checks.
	bool IsHandleValid(const ConstructRef& ref) const;

	/// Returns the earliest SpawnFrame across all live Construct records.
	/// Used by LogicThread to clamp rollback targets — we must never roll back to
	/// before the oldest Construct was spawned, as there is no replay event to re-create it.
	uint32_t GetEarliestSpawnFrame() const;

private:
	// --- Net lookup (private mutable) ---
	friend class ReplicationSystem;

	GlobalConstructHandle LookupGlobalHandle(ConstructNetHandle handle) const
	{
		return NetToRecord.get(handle.GetHandleIndex());
	}

	ConstructRecord* GetRecordPtr(ConstructNetHandle handle);

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
		uint32_t ID          = 0;
		void* Ptr            = nullptr;
		const char* TypeName = nullptr; // typeid(T).name() — static lifetime, safe to store
		std::unique_ptr<StorageBase> Storage;

		// World transition hooks (nullptr if not implemented by the Construct)
		TeardownFn OnTeardown       = nullptr;
		InitializedFn OnInitialized = nullptr;
		ShutdownFn ShutdownPtr      = nullptr;
		ReinitFn ReinitPtr          = nullptr;

		// Collects the live EntityHandle for each registered ConstructView.
		// Set at Create<T> time. Used by ReplicationSystem to build ConstructSpawn
		// payloads for late-joining clients without caching stale handles.
		void (*CollectViewHandlesFn)(void*, std::vector<EntityHandle>&) = nullptr;

		// Net destroy hook — set by ReplicationSystem::RegisterConstruct.
		// Fires before the Entry is erased so ReplicationSystem can queue ConstructDestroy.
		ConstructNetHandle NetHandle{};
		void (*NetDestroyFn)(void* ctx, ConstructNetHandle handle) = nullptr;
		void* NetDestroyCtx                                        = nullptr;
	};

	/// Builds the type-erased Entry shared by Create<T> and CreateForReplication<T>.
	/// Only the T-dependent parts (callbacks, typeid) live here; bookkeeping is in AddEntry.
	template <typename T>
	static Entry MakeEntry(uint32_t id, T* raw, std::unique_ptr<StorageBase> storage)
	{
		Entry entry;
		entry.ID          = id;
		entry.Ptr         = raw;
		entry.TypeName    = typeid(T).name();
		entry.Storage     = std::move(storage);
		entry.ShutdownPtr = [](void* p)
		{
			static_cast<T*>(p)->Shutdown();
		};
		entry.ReinitPtr = [](void* p, WorldBase* w)
		{
			static_cast<T*>(p)->Initialize(w);
		};
		if constexpr (requires(T t) { t.OnWorldTeardown(); })
			entry.OnTeardown = [](void* p)
			{
				static_cast<T*>(p)->OnWorldTeardown();
			};
		if constexpr (requires(T t, WorldBase* w) { t.OnWorldInitialized(w); })
			entry.OnInitialized = [](void* p, WorldBase* w)
			{
				static_cast<T*>(p)->OnWorldInitialized(w);
			};
		return entry;
	}

	/// Non-template core: files the entry into its lifetime bucket.
	void AddEntry(uint8_t lifetimeTier, Entry&& entry);

	static constexpr uint8_t BucketCount = 4; // Level, World, Session, Persistent
	std::vector<Entry> Buckets[BucketCount];
	std::vector<uint32_t> PendingDestructions;
	uint32_t NextID       = 1;
	uint32_t NextNetIndex = 1; // Monotonic counter; 0 is reserved as "invalid"
};
