#pragma once
#include "AssetTypes.h"

#include <string>
#include <unordered_map>

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
// Pin/Unpin is refcounted so multiple worlds can hold the same asset.
// -----------------------------------------------------------------------

struct AssetEntry
{
	AssetID ID;
	std::string Path; // relative to content root (runtime: pak-relative)
	AssetType Type         = AssetType::Invalid;
	AssetFlags Flags       = AssetFlags::None;
	RuntimeFlags State     = RuntimeFlags::None;
	uint32_t SchemaVersion = 0;
	void* Data             = nullptr;
	uint32_t PinCount      = 0; // refcounted — eviction only when 0
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

	// --- Registration (editor/import pipeline) ---
	void Register(const AssetID& id, const std::string& path, AssetType type,
				  uint32_t schemaVersion = 0, AssetFlags flags = AssetFlags::None)
	{
		int64_t uuid        = id.GetUUID();
		AssetEntry& entry   = Entries[uuid];
		entry.ID            = id;
		entry.Path          = path;
		entry.Type          = type;
		entry.SchemaVersion = schemaVersion;
		entry.Flags         = flags;
		entry.State         = RuntimeFlags::None;
	}

	// --- Lookup ---
	const AssetEntry* Find(const AssetID& id) const
	{
		return FindByUUID(ResolveAlias(id.GetUUID()));
	}

	AssetEntry* FindMutable(const AssetID& id)
	{
		return FindMutableByUUID(ResolveAlias(id.GetUUID()));
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

	void Evict(const AssetID& id)
	{
		AssetEntry* e = FindMutable(id);
		if (e && e->PinCount == 0 && e->Data)
		{
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
				entry.Data  = nullptr;
				entry.State = static_cast<RuntimeFlags>(
					static_cast<uint8_t>(entry.State) & ~static_cast<uint8_t>(RuntimeFlags::Loaded));
			}
		}
	}

	// --- Alias ---
	void AddAlias(int64_t fromUUID, int64_t toUUID)
	{
		AliasTable[fromUUID] = toUUID;
	}

	// --- Iteration (editor Content Browser, debug) ---
	const std::unordered_map<int64_t, AssetEntry>& GetAllEntries() const { return Entries; }

	// --- Bulk operations ---
	void Clear()
	{
		Entries.clear();
		AliasTable.clear();
	}

private:
	AssetRegistry()                                = default;
	~AssetRegistry()                               = default;
	AssetRegistry(const AssetRegistry&)            = delete;
	AssetRegistry& operator=(const AssetRegistry&) = delete;

	int64_t ResolveAlias(int64_t uuid) const
	{
		auto it = AliasTable.find(uuid);
		return it != AliasTable.end() ? it->second : uuid;
	}

	const AssetEntry* FindByUUID(int64_t uuid) const
	{
		auto it = Entries.find(uuid);
		return it != Entries.end() ? &it->second : nullptr;
	}

	AssetEntry* FindMutableByUUID(int64_t uuid)
	{
		auto it = Entries.find(uuid);
		return it != Entries.end() ? &it->second : nullptr;
	}

	static bool HasFlag(RuntimeFlags state, RuntimeFlags flag)
	{
		return (static_cast<uint8_t>(state) & static_cast<uint8_t>(flag)) != 0;
	}

	std::unordered_map<int64_t, int64_t> AliasTable;
	std::unordered_map<int64_t, AssetEntry> Entries;
};