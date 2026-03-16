#pragma once
#if !defined(TNX_ENABLE_EDITOR)
#error "AssetDatabase.h requires TNX_ENABLE_EDITOR"
#endif

#include "AssetTypes.h"

#include <string>
#include <unordered_map>
#include <vector>

// -----------------------------------------------------------------------
// AssetSidecar (.tnxid)
//
// Lives next to every source asset. Created once at import, never manually
// edited. Stripped from cooked builds entirely.
// -----------------------------------------------------------------------

enum class SidecarFlags : uint8_t
{
	None           = 0x00,
	Dirty          = 0x01, // content hash mismatch
	SchemaOutdated = 0x02, // importer version changed
};

enum class AssetSidecarState : uint8_t
{
	Clean,
	Dirty,
	SchemaOutdated,
};

struct AssetSidecar
{
	int64_t UUID           = 0;
	uint64_t ContentHash   = 0; // hash of asset bytes at last valid import
	uint32_t SchemaVersion = 0; // importer version that last processed this
	uint8_t Flags          = 0;

	// Read/write a .tnxid file alongside the asset.
	static bool Read(const char* sidecarPath, AssetSidecar& out);
	static bool Write(const char* sidecarPath, const AssetSidecar& sidecar);
};

// -----------------------------------------------------------------------
// AssetDatabaseEntry
//
// One entry per asset in the project. Persisted to AssetDatabase.tnxdb.
// -----------------------------------------------------------------------

struct AssetDatabaseEntry
{
	int64_t UUID = 0;
	std::string Path; // relative to content root
	AssetType Type       = AssetType::Invalid;
	uint64_t ContentHash = 0;
};

// -----------------------------------------------------------------------
// AssetDatabase — Editor-only asset management
//
// Sole authority on UUID<->path mapping. Scans content/ directory,
// reconciles with sidecars, generates UUIDs for new files, writes .tnxdb.
//
// Populates the runtime AssetRegistry so the Content Browser and
// other editor systems can reference assets by ID.
// -----------------------------------------------------------------------

class AssetDatabase
{
public:
	// Scan the content directory and reconcile with existing sidecars.
	// Creates .tnxid files for new assets, marks dirty/missing as needed.
	// Populates the runtime AssetRegistry with all discovered assets.
	void Initialize(const char* contentRoot);

	// Re-scan the content directory (e.g. after file watcher trigger).
	void Reconcile();

	// Persist the database to disk (contentRoot/AssetDatabase.tnxdb).
	bool Save() const;

	// Load from disk. Called by Initialize() if the file exists.
	bool Load();

	// Get all entries for UI display.
	const std::vector<AssetDatabaseEntry>& GetEntries() const { return Entries; }

	// Lookup by UUID.
	const AssetDatabaseEntry* FindByUUID(int64_t uuid) const;

	// Lookup by relative path.
	const AssetDatabaseEntry* FindByPath(const std::string& relativePath) const;

private:
	// Generate a random 40-bit UUID (masked to the UUID bit range of AssetID).
	static int64_t GenerateUUID();

	// Hash file contents for integrity checking.
	static uint64_t HashFileContents(const char* filePath);

	// Build the .tnxid sidecar path from an asset path.
	static std::string SidecarPath(const std::string& assetPath);

	// Derive AssetType from file extension.
	static AssetType TypeFromPath(const std::string& path);

	// Sync all entries to the runtime AssetRegistry.
	void PublishToRegistry() const;

	std::string ContentRoot;
	std::vector<AssetDatabaseEntry> Entries;
	std::unordered_map<int64_t, size_t> UUIDIndex;     // UUID -> Entries index
	std::unordered_map<std::string, size_t> PathIndex; // relative path -> Entries index
};