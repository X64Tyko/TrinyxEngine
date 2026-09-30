#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "AssetTypes.h"
#include "TnxName.h"
#include "Events.h"

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
	TnxName Name;     // FNV1a hash + owned string
	std::string Path; // relative to content root (runtime: pak-relative)
	AssetType Type = AssetType::Invalid;

	// For Mesh/Skeleton entries: the name of the default material that ships with this mesh.
	TnxName DefaultMaterial{};

	AssetFlags Flags       = AssetFlags::None;
	RuntimeFlags State     = RuntimeFlags::None;
	uint32_t SchemaVersion = 0;
	void* Data             = nullptr; // manager slot as uintptr_t (slot-based managers: Mesh, Audio)
	void* CPUData          = nullptr; // typed CPU asset ptr (Skeleton, Animation); stable while loaded
	uint32_t PinCount      = 0;       // refcounted — eviction only when 0

	// Fired with the resolved slotID when the asset finishes loading.
	// One-shot per load event: cleared after firing so stale listeners
	// never accumulate. Bindings with the same context object are safe
	// to add again after re-checkout.
	MultiCallback<void, true, 32, uint32_t> OnLoaded;

	// Fired just before an asset is evicted from its slot.
	// Persistent: survives load/reload cycles. Owners use this to
	// invalidate cached slot indices and request a new checkout.
	MultiCallback<void, true, 64> OnEvicted;
};

// -----------------------------------------------------------------------
// PendingCheckout — queued during entity init lambda, drained by Create<T>
//
// CMeshRef::SetMesh / SetMaterial push entries here instead of calling
// Checkout() directly; Create<T>(Fn&&) drains the list after the lambda
// returns so callbacks are bound with a stable slab pointer and consistent
// entity context. The thread_local list is cleared after each drain.
// -----------------------------------------------------------------------

DEFINE_CALLBACK(AssetLoad, uint32_t)
DEFINE_CALLBACK_VOID(AssetEvict)

struct PendingCheckout
{
	AssetLoad OnLoaded;
	AssetEvict OnEvicted;
	AssetID ID;
};

// -----------------------------------------------------------------------
// AssetDataRef<T> — non-owning typed view into a registry asset's CPU data.
//
// Holds a stable pointer to the AssetEntry (entries live until Unregister,
// not just until eviction). Get() re-checks CPUData on every call: returns
// nullptr once the asset is evicted, valid pointer while it is loaded.
//
// Store as a member for per-tick access without repeated name lookups.
// Does NOT hold a pin — use Checkout if you need eviction prevention.
// -----------------------------------------------------------------------
template <typename T>
class AssetDataRef
{
public:
	AssetDataRef() = default;

	const T* Get() const
	{
		return (EntryPtr && EntryPtr->CPUData) ? static_cast<const T*>(EntryPtr->CPUData) : nullptr;
	}

	explicit operator bool() const { return Get() != nullptr; }
	const T* operator->() const { return Get(); }
	const T& operator*() const { return *Get(); }

private:
	friend class AssetRegistry;
	const AssetEntry* EntryPtr = nullptr;
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

	/// Sets the absolute content root and triggers ValidateAll() on any already-registered entries.
	void SetContentRoot(const std::string& root);
	const std::string& GetContentRoot() const { return ContentRoot; }

	// Resolve an entry's relative path to an absolute filesystem path.
	// Returns an empty string if the entry has no path or ContentRoot is unset.
	std::string ResolvePath(const AssetEntry& entry) const;

	// Convenience: resolve by AssetID.
	std::string ResolvePath(const AssetID& id) const;

	// Convenience: resolve by typed display name — required when multiple asset types share a name.
	std::string ResolvePathByTNameAndType(TnxName name, AssetType type) const;

	// --- Registration (editor/import pipeline) ---
	/// Registers the entry and validates it immediately if ContentRoot is set.
	void Register(const AssetID& id, TnxName name, const std::string& path,
		AssetType type, uint32_t schemaVersion = 0, AssetFlags flags = AssetFlags::None);

	/// Checks file existence on disk; sets/clears @c RuntimeFlags::Missing. Returns true if valid.
	/// Entries with no path (code-registered types) are always considered valid.
	bool Validate(const AssetID& id);

	/// Validates all registered entries. Called by SetContentRoot(); call again after a reconcile.
	void ValidateAll();

	/// Removes an entry from the registry entirely, including its name and slot mappings.
	void Unregister(const AssetID& id);

	// --- Lookup ---
	FORCE_INLINE const AssetEntry* Find(const AssetID& id) const
	{
		return FindInternal(ResolveAlias(id));
	}

	// Type-filtered name lookup — resolves correctly when multiple assets share a display name.
	// Key: (nameHash << 8) | type — unique per (name, AssetType) pair.
	const AssetEntry* FindByTNameAndType(TnxName name, AssetType type) const;

	// Returns all assets that share a display name, one per registered asset type.
	// Queries each known asset type — per-type uniqueness is enforced at Register() time.
	std::vector<const AssetEntry*> GetAssetsByName(TnxName name) const;

	AssetEntry* FindMutable(const AssetID& id)
	{
		return FindMutableByID(ResolveAlias(id));
	}

	// --- State queries ---
	bool IsLoaded(const AssetID& id) const;

	bool IsStreaming(const AssetID& id) const;

	bool IsPinned(const AssetID& id) const;

	// --- Typed CPU-asset access ---
	//
	// Returns an AssetDataRef<T> backed by the entry's CPUData field.
	// CPUData is set synchronously by managers that retain a CPU copy
	// (Skeleton, Animation). Mesh and Audio use slot-only Data.
	// Get() returns nullptr if the asset is not loaded or has no CPU copy.

	template <typename T>
	AssetDataRef<T> GetAssetData(AssetID id) const
	{
		AssetDataRef<T> ref;
		ref.EntryPtr = Find(id);
		return ref;
	}

	template <typename T>
	AssetDataRef<T> GetAssetData(TnxName name) const
	{
		AssetDataRef<T> ref;
		ref.EntryPtr = FindByTNameAndType(name, AssetTypeOf<T>);
		return ref;
	}

	// Slot-based lookup — used when only a slot index is available (e.g., from slab fields).
	template <typename T>
	AssetDataRef<T> GetAssetData(AssetType type, uint32_t slot) const
	{
		AssetDataRef<T> ref;
		uint64_t key = (static_cast<uint64_t>(type) << 32) | slot;
		auto it      = SlotIndex.find(key);
		if (it != SlotIndex.end()) ref.EntryPtr = FindInternal(it->second);
		return ref;
	}

	// --- Memory management (refcounted) ---
	// Multiple worlds/systems can Pin the same asset. Eviction is only
	// valid when PinCount reaches zero.
	void Pin(const AssetID& id);

	void Unpin(const AssetID& id);

	/// Load registry metadata from a cooked AssetDatabase.tnxdb in @p contentRoot.
	/// Does not load asset data — only registers name/path/type/UUID so assets can be
	/// found by name and demand-loaded via Checkout(). Safe to call in non-editor builds.
	void LoadManifest(const char* contentRoot);

	/// Trigger demand load without pinning. Use for pre-warming assets before entity spawn.
	/// Has no effect if the asset is already loaded or no loader is registered for its type.
	void TriggerLoad(const AssetID& id);

	template <AssetType TType>
	void TriggerLoad(TnxName name)
	{
		const AssetEntry* e = FindByTNameAndType(name, TType);
		if (e) TriggerLoad(e->ID);
	}

	// --- Loader registry — demand-load on first Checkout ---
	//
	// Register one loader per AssetType. The loader is called synchronously the
	// first time Checkout() is called for an unloaded asset of that type.
	// Loaders are plain function pointers + context so they compose with lambdas
	// without heap allocation.
	void RegisterLoader(AssetType type, void (*fn)(void*, AssetID), void* ctx);

	// --- GPU upload checker registry ---
	//
	// Asset managers that perform async GPU uploads register a checker here at
	// Initialize() time. IsUploadComplete() returns true only when all registered
	// checkers agree. Callers (e.g., the render thread before first draw) can spin
	// on this or call the individual manager FlushPendingUploads() to block.
	void RegisterUploadChecker(bool (*fn)(void*), void* ctx);

	bool IsUploadComplete() const;

	// --- Checkout / Checkin — callback-driven asset lifetime ---
	//
	// Checkout increments PinCount and registers OnLoaded/OnEvicted callbacks
	// identified by a context pointer (the owning entity record, Construct, etc).
	// If the asset is already loaded, OnLoaded fires immediately with the current
	// slot index. OnEvicted is persistent and will fire on any future eviction.
	// If a loader is registered and the asset is not yet loaded, it is triggered
	// synchronously before this call returns.
	//
	// Checkin decrements PinCount and removes all callbacks bound to bindCtx from
	// both OnLoaded and OnEvicted — works regardless of whether Bind or BindStatic
	// was used to register the callback.
	void Checkout(const AssetID& id,
		Callback<void, uint32_t> onLoaded,
		Callback<void> onEvicted = {});

	void Checkin(const AssetID& id, void* bindCtx);

	// O(1) checkin by (type, slot) — used by Registry::ProcessDeferredDestructions.
	// Requires RegisterSlot to have been called when the asset was committed.
	void CheckinBySlot(AssetType type, uint32_t slot, void* bindCtx);

	// Called by asset managers (MeshManager, AudioManager) when a slot is committed.
	// Registers the (type, slot) → EntryKey reverse mapping used by CheckinBySlot.
	void RegisterSlot(AssetType type, uint32_t slot, const AssetID& id);

	// Called on eviction to remove the reverse mapping.
	void UnregisterSlot(AssetType type, uint32_t slot);

	// --- Init-lambda asset checkout ---
	//
	// Call RegisterPendingCheckout() from component assignment operators during an entity
	// init lambda (e.g., CMeshRef::SetMesh). Create<T>(Fn&&) calls DrainPendingCheckouts()
	// after the lambda to wire all callbacks at once with a stable context.
	//
	// Callers own callback construction. Using the field slab pointer as bindObj allows
	// Checkin(assetID, fieldPtr) at despawn to cleanly unbind without entity-level context.

	// Runtime-typed overload — used by EntityBuilder where type is known at runtime from field metadata.
	static void RegisterPendingCheckout(TnxName name, AssetType type, AssetLoad onLoaded, AssetEvict onEvicted = {});

	// Compile-time-typed overload — preferred when the asset type is statically known.
	template <AssetType TType>
	static void RegisterPendingCheckout(TnxName name, AssetLoad onLoaded, AssetEvict onEvicted = {})
	{
		const AssetEntry* entry = Get().FindByTNameAndType(name, TType);
		if (!entry)
		{
			LOG_ENG_WARN_F("AssetRegistry::RegisterPendingCheckout - asset '%s' (type: %s) not found in registry",
				name.GetStr(), AssetTypeName(TType));
			return;
		}
		PendingList().push_back({ onLoaded, onEvicted, entry->ID });
	}

	void DrainPendingCheckouts();

	void Evict(const AssetID& id);

	void EvictUnpinned();

	// --- Alias ---
	void AddAlias(const AssetID& from, const AssetID& to);

	// --- Iteration (editor Content Browser, debug) ---
	const std::unordered_map<AssetID, AssetEntry, AssetIDHash>& GetAllEntries() const { return Entries; }

	// --- Bulk operations ---
	void Clear();

private:
	AssetRegistry()                                = default;
	~AssetRegistry()                               = default;
	AssetRegistry(const AssetRegistry&)            = delete;
	AssetRegistry& operator=(const AssetRegistry&) = delete;

	// Thread-local pending list shared between RegisterPendingCheckout and DrainPendingCheckouts.
	// Lives in a single getter so both functions operate on the same storage.
	static std::vector<PendingCheckout>& PendingList();

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

	struct AssetLoader
	{
		void (*Fn)(void*, AssetID) = nullptr;
		void* Ctx                  = nullptr;
	};
	struct UploadChecker
	{
		bool (*Fn)(void*) = nullptr;
		void* Ctx         = nullptr;
	};

	static constexpr uint8_t kMaxUploadCheckers = 8;

	std::string ContentRoot;
	AssetLoader Loaders[256];
	UploadChecker UploadCheckers[kMaxUploadCheckers]{};
	uint8_t UploadCheckerCount = 0;
	std::unordered_map<AssetID, AssetID, AssetIDHash> AliasTable;
	std::unordered_map<AssetID, AssetEntry, AssetIDHash> Entries;
	std::unordered_map<uint64_t, AssetID> TypedNameIndex; // (nameHash<<8|type) → AssetID; unique per (name, type)
	std::unordered_map<uint64_t, AssetID> SlotIndex;      // (type<<32|slot) → AssetID
};

// -----------------------------------------------------------------------
// AssetTypeTraits specialisations — forward-declared types; full definitions
// live in their respective manager/asset headers.
// -----------------------------------------------------------------------
struct MeshAsset;
struct SkeletonAsset;
struct AnimationAsset;
struct SoundAsset;

template <>
struct AssetTypeTraits<MeshAsset>
{
	static constexpr AssetType Type = AssetType::Mesh;
};
template <>
struct AssetTypeTraits<SkeletonAsset>
{
	static constexpr AssetType Type = AssetType::Skeleton;
};
template <>
struct AssetTypeTraits<AnimationAsset>
{
	static constexpr AssetType Type = AssetType::Animation;
};
template <>
struct AssetTypeTraits<SoundAsset>
{
	static constexpr AssetType Type = AssetType::Audio;
};
