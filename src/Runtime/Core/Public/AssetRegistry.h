#pragma once
#include "AssetTypes.h"
#include "TnxName.h"
#include "Events.h"

#include <string>
#include <unordered_map>
#include <vector>

// -----------------------------------------------------------------------
// AssetRegistry — Runtime asset resolution
//
// Ingests a cooked manifest at startup. Never generates UUIDs. Never
// touches the filesystem directly — all I/O goes through the loader.
//
// Every asset operation in the engine goes through this registry.
// No raw file paths at runtime, only AssetIDs.
//
// Shared across PIE world instances — asset data is immutable.
// Checkout/Checkin refcounts slots so multiple owners can hold the same
// asset; eviction is only valid when PinCount reaches zero.
// -----------------------------------------------------------------------

struct AssetEntry
{
	AssetID ID;
	TnxName Name;     // FNV1a hash + owned string — primary key in NameIndex
	std::string Path; // relative to content root (runtime: pak-relative)
	AssetType Type         = AssetType::Invalid;

	// For StaticMesh/SkeletalMesh entries: the name of the default material that ships with
	// this mesh.
	TnxName DefaultMaterial{};

	AssetFlags Flags       = AssetFlags::None;
	RuntimeFlags State     = RuntimeFlags::None;
	uint32_t SchemaVersion = 0;
	void* Data             = nullptr;
	uint32_t PinCount      = 0; // refcounted — eviction only when 0

	// Fired with the resolved slotID when the asset finishes loading.
	// One-shot per load event: cleared after firing so stale listeners
	// never accumulate. Bindings with the same context object are safe
	// to add again after re-checkout.
	FixedMultiCallback<void, 32, uint32_t> OnLoaded;

	// Fired just before an asset is evicted from its slot.
	// Persistent: survives load/reload cycles. Owners use this to
	// invalidate cached slot indices and request a new checkout.
	FixedMultiCallback<void, 64> OnEvicted;
};

// -----------------------------------------------------------------------
// PendingCheckout — queued during entity init lambda, drained by Create<T>
//
// CMeshRef::SetMesh / SetMaterial push entries here instead of calling
// Checkout() directly; Create<T>(Fn&&) drains the list after the lambda
// returns so callbacks are bound with a stable slab pointer and consistent
// entity context. The thread_local list is cleared after each drain.
// -----------------------------------------------------------------------

struct PendingCheckout
{
	uint32_t* FieldPtr; // stable slab pointer — written by onLoaded, cleared by onEvicted
	AssetID ID;
	bool Conditional; // if true, skip when *FieldPtr != 0 (field already set by user)
};

class AssetRegistry
{
public:
	static AssetRegistry& Get()
	{
		static AssetRegistry instance;
		return instance;
	}

	// --- Startup ---
	// void IngestManifest(const AssetManifest& manifest);  // TODO: cooked manifest

	// Content root — absolute path to the content directory.
	// Must be set before any ResolvePath call. Called by AssetDatabase::Initialize()
	// (editor) and by TrinyxEngine startup (runtime, from EngineConfig::ProjectDir).
	void SetContentRoot(const std::string& root) { ContentRoot = root; }
	const std::string& GetContentRoot() const { return ContentRoot; }

	// Resolve an entry's relative path to an absolute filesystem path.
	// Returns an empty string if the entry has no path or ContentRoot is unset.
	std::string ResolvePath(const AssetEntry& entry) const
	{
		if (entry.Path.empty() || ContentRoot.empty()) return {};
		return ContentRoot + "/" + entry.Path;
	}

	// Convenience: resolve by AssetID.
	std::string ResolvePath(const AssetID& id) const
	{
		const AssetEntry* e = Find(id);
		return e ? ResolvePath(*e) : std::string{};
	}

	// Convenience: resolve by display name (TnxName).
	std::string ResolvePathByTName(TnxName name) const
	{
		const AssetEntry* e = FindByTName(name);
		return e ? ResolvePath(*e) : std::string{};
	}

	// Convenience: resolve by display name (string shim).
	std::string ResolvePathByName(const std::string& name) const
	{
		return ResolvePathByTName(TnxName(name.c_str()));
	}

	// --- Registration (editor/import pipeline) ---
	void Register(const AssetID& id, const std::string& name, const std::string& path,
				  AssetType type, uint32_t schemaVersion = 0, AssetFlags flags = AssetFlags::None)
	{
		AssetEntry& entry   = Entries[id];
		entry.ID            = id;
		entry.Name          = TnxName(name.c_str());
		entry.Path          = path;
		entry.Type          = type;
		entry.SchemaVersion = schemaVersion;
		entry.Flags         = flags;
		entry.State         = RuntimeFlags::None;

		if (!name.empty()) NameIndex[entry.Name.Value] = id;
	}

	// --- Lookup ---
	FORCE_INLINE const AssetEntry* Find(const AssetID& id) const
	{
		return FindInternal(ResolveAlias(id));
	}

	// Primary name lookup — O(1) hash compare.
	const AssetEntry* FindByTName(TnxName name) const
	{
		auto it = NameIndex.find(name.Value);
		return it != NameIndex.end() ? Find(it->second) : nullptr;
	}

	// String shim — hashes at call time, delegates to FindByTName.
	const AssetEntry* FindByName(const std::string& name) const
	{
		return FindByTName(TnxName(name.c_str()));
	}

	AssetEntry* FindMutable(const AssetID& id)
	{
		return FindMutableByID(ResolveAlias(id));
	}

	// --- State queries ---
	bool IsLoaded(const AssetID& id) const
	{
		const AssetEntry* e = Find(id);
		return e && HasFlag(e->State, RuntimeFlags::Loaded);
	}

	bool IsStreaming(const AssetID& id) const
	{
		const AssetEntry* e = Find(id);
		return e && HasFlag(e->State, RuntimeFlags::Streaming);
	}

	bool IsPinned(const AssetID& id) const
	{
		const AssetEntry* e = Find(id);
		return e && e->PinCount > 0;
	}

	// --- Memory management (refcounted) ---
	// Multiple worlds/systems can Pin the same asset. Eviction is only
	// valid when PinCount reaches zero.
	void Pin(const AssetID& id)
	{
		AssetEntry* e = FindMutable(id);
		if (e) ++e->PinCount;
	}

	void Unpin(const AssetID& id)
	{
		AssetEntry* e = FindMutable(id);
		if (e && e->PinCount > 0) --e->PinCount;
	}

	// --- Checkout / Checkin — callback-driven asset lifetime ---
	//
	// Checkout increments PinCount and registers OnLoaded/OnEvicted callbacks
	// identified by a context pointer (the owning entity record, Construct, etc).
	// If the asset is already loaded, OnLoaded fires immediately with the current
	// slot index. OnEvicted is persistent and will fire on any future eviction.
	//
	// Checkin decrements PinCount and removes all callbacks bound to bindCtx from
	// both OnLoaded and OnEvicted — works regardless of whether Bind or BindStatic
	// was used to register the callback.
	void Checkout(const AssetID& id,
				  Callback<void, uint32_t> onLoaded,
				  Callback<void> onEvicted = {})
	{
		AssetEntry* e = FindMutable(id);
		if (!e) return;

		++e->PinCount;

		if (onLoaded.IsBound())
		{
			if (HasFlag(e->State, RuntimeFlags::Loaded))
			{
				// Already available — fire immediately, don't register.
				uint32_t slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->Data));
				onLoaded(slot);
			}
			else
			{
				e->OnLoaded.BindStatic(onLoaded.stub, onLoaded.bindObj);
			}
		}

		if (onEvicted.IsBound())
		{
			e->OnEvicted.BindStatic(onEvicted.stub, onEvicted.bindObj);
		}
	}

	void Checkin(const AssetID& id, void* bindCtx)
	{
		AssetEntry* e = FindMutable(id);
		if (!e) return;

		e->OnLoaded.UnbindByContext(bindCtx);
		e->OnEvicted.UnbindByContext(bindCtx);

		if (e->PinCount > 0) --e->PinCount;
	}

	// O(1) checkin by (type, slot) — used by Registry::ProcessDeferredDestructions.
	// Requires RegisterSlot to have been called when the asset was committed.
	void CheckinBySlot(AssetType type, uint32_t slot, void* bindCtx)
	{
		uint64_t key = (static_cast<uint64_t>(type) << 32) | slot;
		auto it      = SlotIndex.find(key);
		if (it == SlotIndex.end()) return;

		AssetEntry* e = FindMutableByID(it->second);
		if (!e) return;

		e->OnLoaded.UnbindByContext(bindCtx);
		e->OnEvicted.UnbindByContext(bindCtx);
		if (e->PinCount > 0) --e->PinCount;
	}

	// Called by asset managers (MeshManager, AudioManager) when a slot is committed.
	// Registers the (type, slot) → EntryKey reverse mapping used by CheckinBySlot.
	void RegisterSlot(AssetType type, uint32_t slot, const AssetID& id)
	{
		uint64_t key   = (static_cast<uint64_t>(type) << 32) | slot;
		SlotIndex[key] = id;
	}

	// Called on eviction to remove the reverse mapping.
	void UnregisterSlot(AssetType type, uint32_t slot)
	{
		uint64_t key = (static_cast<uint64_t>(type) << 32) | slot;
		SlotIndex.erase(key);
	}

	// --- Init-lambda asset checkout ---
	//
	// Call RegisterPendingCheckout() from component assignment operators during an entity
	// init lambda (e.g., CMeshRef::SetMesh). Create<T>(Fn&&) calls DrainPendingCheckouts()
	// after the lambda to wire all callbacks at once with a stable context.
	//
	// Both callbacks use the field slab pointer (FieldPtr) as their bindObj so that
	// Checkin(assetID, fieldPtr) at despawn cleanly unbinds without needing a separate
	// entity-level context.

	static void RegisterPendingCheckout(uint32_t* fieldPtr, TnxName name, bool conditional = false)
	{
		const AssetEntry* entry = Get().FindByTName(name);
		if (!entry)
		{
			LOG_ENG_WARN_F("AssetRegistry::RegisterPendingCheckout - asset '%s' not found in registry", name.GetStr());
			return;
		}
		PendingList().push_back({fieldPtr, entry->ID, conditional});
	}

	void DrainPendingCheckouts()
	{
		auto& pending = PendingList();
		for (const PendingCheckout& pc : pending)
		{
			if (pc.Conditional && *pc.FieldPtr != 0) continue;

			Callback<void, uint32_t> onLoaded;
			onLoaded.BindStatic(
				[](void* ctx, uint32_t slot) { *static_cast<uint32_t*>(ctx) = slot; },
				pc.FieldPtr);

			Callback<void> onEvicted;
			onEvicted.BindStatic(
				[](void* ctx) { *static_cast<uint32_t*>(ctx) = 0; },
				pc.FieldPtr);

			Checkout(pc.ID, onLoaded, onEvicted);
		}
		pending.clear();
	}

	void Evict(const AssetID& id)
	{
		AssetEntry* e = FindMutable(id);
		if (e && e->PinCount == 0 && e->Data)
		{
			uint32_t slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->Data));
			UnregisterSlot(e->Type, slot);
			e->OnEvicted();
			e->OnLoaded.Reset();
			// TODO: type-specific deallocation
			e->Data  = nullptr;
			e->State = static_cast<RuntimeFlags>(
				static_cast<uint8_t>(e->State) & ~static_cast<uint8_t>(RuntimeFlags::Loaded));
		}
	}

	void EvictUnpinned()
	{
		for (auto& [uuid, entry] : Entries)
		{
			if (entry.PinCount == 0 && entry.Data)
			{
				uint32_t slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(entry.Data));
				UnregisterSlot(entry.Type, slot);
				entry.OnEvicted();
				entry.OnLoaded.Reset();
				entry.Data  = nullptr;
				entry.State = static_cast<RuntimeFlags>(
					static_cast<uint8_t>(entry.State) & ~static_cast<uint8_t>(RuntimeFlags::Loaded));
			}
		}
	}

	// --- Alias ---
	void AddAlias(const AssetID& from, const AssetID& to)
	{
		AliasTable[from] = to;
	}

	// --- Iteration (editor Content Browser, debug) ---
	const std::unordered_map<AssetID, AssetEntry, AssetIDHash>& GetAllEntries() const { return Entries; }

	// --- Bulk operations ---
	void Clear()
	{
		Entries.clear();
		AliasTable.clear();
		NameIndex.clear();
		SlotIndex.clear();
	}

private:
	AssetRegistry()                                = default;
	~AssetRegistry()                               = default;
	AssetRegistry(const AssetRegistry&)            = delete;
	AssetRegistry& operator=(const AssetRegistry&) = delete;

	// Thread-local pending list shared between RegisterPendingCheckout and DrainPendingCheckouts.
	// Lives in a single getter so both functions operate on the same storage.
	static std::vector<PendingCheckout>& PendingList()
	{
		static thread_local std::vector<PendingCheckout> tl_Pending;
		return tl_Pending;
	}

	FORCE_INLINE AssetID ResolveAlias(const AssetID& key) const
	{
		auto it = AliasTable.find(key);
		return it != AliasTable.end() ? it->second : key;
	}

	FORCE_INLINE const AssetEntry* FindInternal(const AssetID& key) const
	{
		auto it = Entries.find(key);
		return it != Entries.end() ? &it->second : nullptr;
	}

	FORCE_INLINE AssetEntry* FindMutableByID(const AssetID& key)
	{
		auto it = Entries.find(key);
		return it != Entries.end() ? &it->second : nullptr;
	}

	static bool HasFlag(RuntimeFlags state, RuntimeFlags flag)
	{
		return (static_cast<uint8_t>(state) & static_cast<uint8_t>(flag)) != 0;
	}

	std::string ContentRoot;
	std::unordered_map<AssetID, AssetID, AssetIDHash> AliasTable;
	std::unordered_map<AssetID, AssetEntry, AssetIDHash> Entries;
	std::unordered_map<uint32_t, AssetID> NameIndex; // TnxName hash → AssetID
	std::unordered_map<uint64_t, AssetID> SlotIndex; // (type<<32|slot) → AssetID
};
