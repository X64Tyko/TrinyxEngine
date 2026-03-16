#include "AssetDatabase.h"
#include "AssetRegistry.h"
#include "Json.h"
#include "Logger.h"

#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

// -----------------------------------------------------------------------
// AssetSidecar — .tnxid read/write (simple binary for speed)
// -----------------------------------------------------------------------

bool AssetSidecar::Read(const char* sidecarPath, AssetSidecar& out)
{
	std::ifstream file(sidecarPath, std::ios::binary);
	if (!file.is_open()) return false;

	file.read(reinterpret_cast<char*>(&out.UUID), sizeof(out.UUID));
	file.read(reinterpret_cast<char*>(&out.ContentHash), sizeof(out.ContentHash));
	file.read(reinterpret_cast<char*>(&out.SchemaVersion), sizeof(out.SchemaVersion));
	file.read(reinterpret_cast<char*>(&out.Flags), sizeof(out.Flags));

	return file.good();
}

bool AssetSidecar::Write(const char* sidecarPath, const AssetSidecar& sidecar)
{
	std::ofstream file(sidecarPath, std::ios::binary | std::ios::trunc);
	if (!file.is_open()) return false;

	file.write(reinterpret_cast<const char*>(&sidecar.UUID), sizeof(sidecar.UUID));
	file.write(reinterpret_cast<const char*>(&sidecar.ContentHash), sizeof(sidecar.ContentHash));
	file.write(reinterpret_cast<const char*>(&sidecar.SchemaVersion), sizeof(sidecar.SchemaVersion));
	file.write(reinterpret_cast<const char*>(&sidecar.Flags), sizeof(sidecar.Flags));

	return file.good();
}

// -----------------------------------------------------------------------
// AssetDatabase
// -----------------------------------------------------------------------

int64_t AssetDatabase::GenerateUUID()
{
	static std::mt19937_64 rng(std::random_device{}());
	int64_t raw = static_cast<int64_t>(rng());
	// Mask to UUID bit range [47:8]
	return raw & 0x0000FFFFFFFFFF00LL;
}

uint64_t AssetDatabase::HashFileContents(const char* filePath)
{
	std::ifstream file(filePath, std::ios::binary);
	if (!file.is_open()) return 0;

	// FNV-1a 64-bit
	uint64_t hash = 14695981039346656037ULL;
	char buf[4096];
	while (file.read(buf, sizeof(buf)) || file.gcount() > 0)
	{
		for (std::streamsize i = 0; i < file.gcount(); ++i)
		{
			hash ^= static_cast<uint64_t>(static_cast<uint8_t>(buf[i]));
			hash *= 1099511628211ULL;
		}
	}
	return hash;
}

std::string AssetDatabase::SidecarPath(const std::string& assetPath)
{
	return assetPath + ".tnxid";
}

AssetType AssetDatabase::TypeFromPath(const std::string& path)
{
	fs::path p(path);
	std::string ext = p.extension().string();
	if (ext.empty()) return AssetType::Invalid;
	return AssetTypeFromExtension(ext.c_str());
}

void AssetDatabase::Initialize(const char* contentRoot)
{
	ContentRoot = contentRoot;

	// Ensure content directory exists
	if (!fs::exists(ContentRoot))
	{
		fs::create_directories(ContentRoot);
		LOG_INFO_F("[AssetDB] Created content directory: %s", ContentRoot.c_str());
	}

	// Try to load existing database
	Load();

	// Reconcile with filesystem
	Reconcile();

	// Persist any changes (new sidecars, etc.)
	Save();

	// Push to runtime registry
	PublishToRegistry();

	LOG_INFO_F("[AssetDB] Initialized with %zu assets from %s", Entries.size(), ContentRoot.c_str());
}

void AssetDatabase::Reconcile()
{
	// Track which existing entries we've seen (to detect deletions)
	std::vector<bool> seen(Entries.size(), false);

	// Scan filesystem
	for (auto& dirEntry : fs::recursive_directory_iterator(ContentRoot))
	{
		if (!dirEntry.is_regular_file()) continue;

		std::string absPath = dirEntry.path().string();
		std::string ext     = dirEntry.path().extension().string();

		// Skip sidecars, database file, and unknown extensions
		if (ext == ".tnxid" || ext == ".tnxdb") continue;

		AssetType type = AssetTypeFromExtension(ext.c_str());
		if (type == AssetType::Invalid) continue;

		// Relative path from content root
		std::string relPath = fs::relative(dirEntry.path(), ContentRoot).string();

		// Check for existing sidecar
		std::string sidecar = SidecarPath(absPath);
		AssetSidecar sc{};

		if (AssetSidecar::Read(sidecar.c_str(), sc))
		{
			// Sidecar exists — validate content
			uint64_t currentHash = HashFileContents(absPath.c_str());

			// Find by UUID in our database
			auto uuidIt = UUIDIndex.find(sc.UUID);
			if (uuidIt != UUIDIndex.end())
			{
				// Known asset — check for content changes or path moves
				size_t idx = uuidIt->second;
				seen[idx]  = true;

				AssetDatabaseEntry& entry = Entries[idx];

				if (entry.Path != relPath)
				{
					// Asset was moved — update path
					PathIndex.erase(entry.Path);
					entry.Path         = relPath;
					PathIndex[relPath] = idx;
					LOG_INFO_F("[AssetDB] Asset moved: %s -> %s", entry.Path.c_str(), relPath.c_str());
				}

				if (currentHash != sc.ContentHash)
				{
					// Content changed
					entry.ContentHash = currentHash;
					sc.ContentHash    = currentHash;
					sc.Flags          |= static_cast<uint8_t>(SidecarFlags::Dirty);
					AssetSidecar::Write(sidecar.c_str(), sc);
					LOG_INFO_F("[AssetDB] Asset modified: %s", relPath.c_str());
				}
			}
			else
			{
				// Sidecar exists but not in database — re-register
				AssetDatabaseEntry entry;
				entry.UUID        = sc.UUID;
				entry.Path        = relPath;
				entry.Type        = type;
				entry.ContentHash = sc.ContentHash;

				size_t idx = Entries.size();
				Entries.push_back(entry);
				UUIDIndex[entry.UUID] = idx;
				PathIndex[relPath]    = idx;
				seen.push_back(true);

				LOG_INFO_F("[AssetDB] Re-registered: %s", relPath.c_str());
			}
		}
		else
		{
			// No sidecar — new asset, needs import
			auto pathIt = PathIndex.find(relPath);
			if (pathIt != PathIndex.end())
			{
				// Path already known (shouldn't happen without sidecar, but recover)
				seen[pathIt->second] = true;
				continue;
			}

			int64_t uuid  = GenerateUUID();
			uint64_t hash = HashFileContents(absPath.c_str());

			// Write sidecar
			AssetSidecar newSc{};
			newSc.UUID          = uuid;
			newSc.ContentHash   = hash;
			newSc.SchemaVersion = 0;
			newSc.Flags         = 0;
			AssetSidecar::Write(sidecar.c_str(), newSc);

			// Add to database
			AssetDatabaseEntry entry;
			entry.UUID        = uuid;
			entry.Path        = relPath;
			entry.Type        = type;
			entry.ContentHash = hash;

			size_t idx = Entries.size();
			Entries.push_back(entry);
			UUIDIndex[entry.UUID] = idx;
			PathIndex[relPath]    = idx;
			seen.push_back(true);

			LOG_INFO_F("[AssetDB] New asset imported: %s (UUID: %lld)", relPath.c_str(),
					   static_cast<long long>(uuid));
		}
	}

	// Mark missing assets (in database but not on filesystem)
	for (size_t i = 0; i < seen.size(); ++i)
	{
		if (!seen[i])
		{
			LOG_WARN_F("[AssetDB] Asset missing from filesystem: %s", Entries[i].Path.c_str());
		}
	}
}

bool AssetDatabase::Save() const
{
	JsonValue root   = JsonValue::Object();
	JsonValue assets = JsonValue::Array();

	for (auto& entry : Entries)
	{
		JsonValue asset      = JsonValue::Object();
		asset["uuid"]        = JsonValue::Number(static_cast<double>(entry.UUID));
		asset["path"]        = JsonValue::String(entry.Path);
		asset["type"]        = JsonValue::Number(static_cast<double>(static_cast<uint8_t>(entry.Type)));
		asset["contentHash"] = JsonValue::Number(static_cast<double>(entry.ContentHash));
		assets.GetArray().push_back(std::move(asset));
	}

	root["assets"] = std::move(assets);

	std::string dbPath = (fs::path(ContentRoot) / "AssetDatabase.tnxdb").string();
	std::string json   = JsonWrite(root, true);

	std::ofstream file(dbPath, std::ios::trunc);
	if (!file.is_open())
	{
		LOG_ERROR_F("[AssetDB] Failed to write database: %s", dbPath.c_str());
		return false;
	}
	file << json;
	return true;
}

bool AssetDatabase::Load()
{
	std::string dbPath = (fs::path(ContentRoot) / "AssetDatabase.tnxdb").string();

	std::ifstream file(dbPath);
	if (!file.is_open()) return false;

	std::string contents((std::istreambuf_iterator<char>(file)),
						 std::istreambuf_iterator<char>());

	JsonValue root = JsonParse(contents);
	if (!root.IsObject()) return false;

	const JsonValue* assets = root.Find("assets");
	if (!assets || !assets->IsArray()) return false;

	Entries.clear();
	UUIDIndex.clear();
	PathIndex.clear();

	for (auto& item : assets->AsArray())
	{
		if (!item.IsObject()) continue;

		AssetDatabaseEntry entry;

		const JsonValue* uuid = item.Find("uuid");
		const JsonValue* path = item.Find("path");
		const JsonValue* type = item.Find("type");
		const JsonValue* hash = item.Find("contentHash");

		if (!uuid || !path) continue;

		entry.UUID        = static_cast<int64_t>(uuid->AsNumber());
		entry.Path        = path->AsString();
		entry.Type        = type ? static_cast<AssetType>(type->AsInt()) : AssetType::Invalid;
		entry.ContentHash = hash ? static_cast<uint64_t>(hash->AsNumber()) : 0;

		size_t idx = Entries.size();
		Entries.push_back(entry);
		UUIDIndex[entry.UUID] = idx;
		PathIndex[entry.Path] = idx;
	}

	LOG_INFO_F("[AssetDB] Loaded %zu entries from %s", Entries.size(), dbPath.c_str());
	return true;
}

const AssetDatabaseEntry* AssetDatabase::FindByUUID(int64_t uuid) const
{
	auto it = UUIDIndex.find(uuid);
	return it != UUIDIndex.end() ? &Entries[it->second] : nullptr;
}

const AssetDatabaseEntry* AssetDatabase::FindByPath(const std::string& relativePath) const
{
	auto it = PathIndex.find(relativePath);
	return it != PathIndex.end() ? &Entries[it->second] : nullptr;
}

void AssetDatabase::PublishToRegistry() const
{
	AssetRegistry& registry = AssetRegistry::Get();

	for (auto& entry : Entries)
	{
		AssetID id = AssetID::Create(entry.UUID, entry.Type);
		registry.Register(id, entry.Path, entry.Type);
	}
}