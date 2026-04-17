#include "AssetDatabase.h"
#include "AssetRegistry.h"
#include "Json.h"
#include "Logger.h"

#include <filesystem>
#include <fstream>

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

int64_t AssetDatabase::GenerateUUID(AssetType type)
{
	uint8_t typeIdx  = static_cast<uint8_t>(type);
	uint32_t counter = ++NextCounter[typeIdx];
	// Pack counter into UUID bit range [47:8]
	return static_cast<int64_t>(counter) << 8;
}

std::string AssetDatabase::NameFromPath(const std::string& path)
{
	return fs::path(path).stem().string();
}

bool AssetDatabase::Rename(AssetID id, const std::string& newName)
{
	for (auto& entry : Entries)
	{
		if (entry.ID != id) continue;

		entry.Name = newName;

		AssetEntry* rtEntry = AssetRegistry::Get().FindMutable(id);
		if (rtEntry) rtEntry->Name = TnxName(newName.c_str());

		return true;
	}
	return false;
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
		LOG_ENG_INFO_F("[AssetDB] Created content directory: %s", ContentRoot.c_str());
	}

	// Set content root on the runtime registry so paths resolve to absolute paths.
	AssetRegistry::Get().SetContentRoot(ContentRoot);

	// Try to load existing database
	Load();

	// Reconcile with filesystem
	Reconcile();

	// Persist any changes (new sidecars, etc.)
	Save();

	// Push to runtime registry
	PublishToRegistry();

	LOG_ENG_INFO_F("[AssetDB] Initialized with %zu assets from %s", Entries.size(), ContentRoot.c_str());
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

			auto pathIt = PathIndex.find(relPath);
			if (pathIt != PathIndex.end())
			{
				// Known asset — check for content changes
				size_t idx = pathIt->second;
				seen[idx]  = true;

				AssetDatabaseEntry& entry = Entries[idx];

				if (entry.Path != relPath)
				{
					// Asset was moved — update path
					PathIndex.erase(entry.Path);
					entry.Path         = relPath;
					PathIndex[relPath] = idx;
					LOG_ENG_INFO_F("[AssetDB] Asset moved: %s -> %s", entry.Path.c_str(), relPath.c_str());
				}

				if (currentHash != sc.ContentHash)
				{
					// Content changed
					entry.ContentHash = currentHash;
					sc.ContentHash    = currentHash;
					sc.Flags          |= static_cast<uint8_t>(SidecarFlags::Dirty);
					AssetSidecar::Write(sidecar.c_str(), sc);
					LOG_ENG_INFO_F("[AssetDB] Asset modified: %s", relPath.c_str());
				}
			}
			else
			{
				// Sidecar exists but not in database — re-register
				AssetDatabaseEntry entry;
				entry.ID          = AssetID::Create(sc.UUID, type);
				entry.Path        = relPath;
				entry.Type        = type;
				entry.ContentHash = sc.ContentHash;

				size_t idx = Entries.size();
				Entries.push_back(entry);
				PathIndex[relPath] = idx;
				seen.push_back(true);

				LOG_ENG_INFO_F("[AssetDB] Re-registered: %s", relPath.c_str());
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

			int64_t uuid  = GenerateUUID(type);
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
			entry.ID          = AssetID::Create(uuid, type);
			entry.Name        = NameFromPath(relPath);
			entry.Path        = relPath;
			entry.Type        = type;
			entry.ContentHash = hash;

			size_t idx = Entries.size();
			Entries.push_back(entry);
			PathIndex[relPath] = idx;
			seen.push_back(true);

			LOG_ENG_INFO_F("[AssetDB] New asset imported: %s (UUID: %lld)", relPath.c_str(),
						   static_cast<long long>(uuid));
		}
	}

	// Mark missing assets (in database but not on filesystem)
	for (size_t i = 0; i < seen.size(); ++i)
	{
		if (!seen[i])
		{
			LOG_ENG_WARN_F("[AssetDB] Asset missing from filesystem: %s", Entries[i].Path.c_str());
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
		asset["uuid"]        = JsonValue::Number(static_cast<double>(entry.ID.GetUUID()));
		asset["name"]        = JsonValue::String(entry.Name);
		asset["path"]        = JsonValue::String(entry.Path);
		asset["type"]        = JsonValue::Number(static_cast<double>(static_cast<uint8_t>(entry.Type)));
		asset["contentHash"] = JsonValue::Number(static_cast<double>(entry.ContentHash));
		assets.GetArray().push_back(std::move(asset));
	}

	root["assets"] = std::move(assets);

	// Persist per-type counters
	JsonValue counters = JsonValue::Object();
	for (int i = 0; i < 256; ++i)
	{
		if (NextCounter[i] > 0)
		{
			std::string key = std::to_string(i);
			counters[key]   = JsonValue::Number(static_cast<double>(NextCounter[i]));
		}
	}
	root["counters"] = std::move(counters);

	std::string dbPath = (fs::path(ContentRoot) / "AssetDatabase.tnxdb").string();
	std::string json   = JsonWrite(root, true);

	std::ofstream file(dbPath, std::ios::trunc);
	if (!file.is_open())
	{
		LOG_ENG_ERROR_F("[AssetDB] Failed to write database: %s", dbPath.c_str());
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
	PathIndex.clear();

	for (auto& item : assets->AsArray())
	{
		if (!item.IsObject()) continue;

		AssetDatabaseEntry entry;

		const JsonValue* uuid = item.Find("uuid");
		const JsonValue* name = item.Find("name");
		const JsonValue* path = item.Find("path");
		const JsonValue* type = item.Find("type");
		const JsonValue* hash = item.Find("contentHash");

		if (!uuid || !path) continue;

		entry.ID = AssetID::Create(static_cast<int64_t>(uuid->AsNumber()),
								   type ? static_cast<AssetType>(type->AsInt()) : AssetType::Invalid);
		entry.Name        = (name && name->IsString()) ? name->AsString() : NameFromPath(path->AsString());
		entry.Path        = path->AsString();
		entry.Type        = entry.ID.GetType();
		entry.ContentHash = hash ? static_cast<uint64_t>(hash->AsNumber()) : 0;

		size_t idx = Entries.size();
		Entries.push_back(entry);
		PathIndex[entry.Path] = idx;
	}

	// Load per-type counters
	const JsonValue* counters = root.Find("counters");
	if (counters && counters->IsObject())
	{
		for (auto& [key, val] : counters->AsObject())
		{
			int typeIdx = std::stoi(key);
			if (typeIdx >= 0 && typeIdx < 256) NextCounter[typeIdx] = static_cast<uint32_t>(val.AsInt());
		}
	}

	// Recover counters from existing entries (handles legacy databases without counters)
	for (auto& entry : Entries)
	{
		uint8_t typeIdx = static_cast<uint8_t>(entry.Type);
		uint32_t seq    = static_cast<uint32_t>((entry.ID.GetUUID() >> 8) & 0xFFFFFFFF);
		if (seq >= NextCounter[typeIdx]) NextCounter[typeIdx] = seq + 1;
	}

	LOG_ENG_INFO_F("[AssetDB] Loaded %zu entries from %s", Entries.size(), dbPath.c_str());
	return true;
}

const AssetDatabaseEntry* AssetDatabase::FindByID(AssetID id) const
{
	for (const auto& entry : Entries)
		if (entry.ID == id) return &entry;
	return nullptr;
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
		AssetID id = entry.ID;
		registry.Register(id, entry.Name, entry.Path, entry.Type);
	}
}