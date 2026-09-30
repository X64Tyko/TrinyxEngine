#include "AssetRegistry.h"
#include "Json.h"
#include "Logger.h"

#include <filesystem>
#include <fstream>

void AssetRegistry::LoadManifest(const char* contentRoot)
{
	if (!contentRoot || contentRoot[0] == '\0') return;

	std::string dbPath = std::string(contentRoot) + "/AssetDatabase.tnxdb";
	std::ifstream file(dbPath);
	if (!file.is_open())
	{
		LOG_ENG_WARN_F("[AssetRegistry] No manifest found at %s", dbPath.c_str());
		return;
	}

	std::string contents((std::istreambuf_iterator<char>(file)),
		std::istreambuf_iterator<char>());
	JsonValue root = JsonParse(contents);
	if (!root.IsObject()) return;

	const JsonValue* assets = root.Find("assets");
	if (!assets || !assets->IsArray()) return;

	size_t count = 0;
	for (const auto& item : assets->AsArray())
	{
		if (!item.IsObject()) continue;

		const JsonValue* uuid = item.Find("uuid");
		const JsonValue* name = item.Find("name");
		const JsonValue* path = item.Find("path");
		const JsonValue* type = item.Find("type");

		if (!uuid || !path || !path->IsString()) continue;

		AssetID id = AssetID::Create(
			static_cast<int64_t>(uuid->AsNumber()),
			type ? static_cast<AssetType>(type->AsInt()) : AssetType::Invalid);

		std::string nameStr = (name && name->IsString()) ? name->AsString() : std::string{};
		if (nameStr.empty())
			nameStr = std::filesystem::path(path->AsString()).stem().string();

		Register(id, TnxName(nameStr.c_str()), path->AsString(), id.GetType());
		++count;
	}

	LOG_ENG_INFO_F("[AssetRegistry] Loaded manifest: %zu assets from %s", count, dbPath.c_str());
}

void AssetRegistry::SetContentRoot(const std::string& root)
{
	ContentRoot = root;
	if (!Entries.empty())
		ValidateAll();
}

void AssetRegistry::Register(const AssetID& id, TnxName name, const std::string& path,
	AssetType type, uint32_t schemaVersion, AssetFlags flags)
{
	AssetEntry& entry   = Entries[id];
	entry.ID            = id;
	entry.Name          = name;
	entry.Path          = path;
	entry.Type          = type;
	entry.SchemaVersion = schemaVersion;
	entry.Flags         = flags;
	entry.State         = RuntimeFlags::None;

	if (name.IsValid())
	{
		uint64_t key        = (static_cast<uint64_t>(name.Value) << 8) | static_cast<uint8_t>(type);
		auto [it, inserted] = TypedNameIndex.emplace(key, id);
		if (!inserted)
			LOG_ENG_WARN_F("[AssetRegistry] Duplicate name '%s' for type %s — overwriting existing entry",
				name.GetStr(), AssetTypeName(type));
		it->second = id;
	}

	if (!ContentRoot.empty() && !path.empty())
		Validate(id);
}

bool AssetRegistry::Validate(const AssetID& id)
{
	AssetEntry* e = FindMutableByID(ResolveAlias(id));
	if (!e) return false;

	if (e->Path.empty() || ContentRoot.empty())
		return true;

	const bool exists = std::filesystem::exists(
		std::filesystem::path(ContentRoot) / e->Path);

	if (exists)
	{
		e->State = static_cast<RuntimeFlags>(
			static_cast<uint8_t>(e->State) & ~static_cast<uint8_t>(RuntimeFlags::Missing));
	}
	else
	{
		e->State = static_cast<RuntimeFlags>(
			static_cast<uint8_t>(e->State) | static_cast<uint8_t>(RuntimeFlags::Missing));
		LOG_ENG_WARN_F("[AssetRegistry] Missing: '%s' (%s)", e->Path.c_str(), e->Name.GetStr());
	}

	return exists;
}

void AssetRegistry::ValidateAll()
{
	for (auto& [id, entry] : Entries)
		Validate(id);
}

void AssetRegistry::Unregister(const AssetID& id)
{
	auto it = Entries.find(id);
	if (it == Entries.end()) return;

	const AssetEntry& entry = it->second;
	if (entry.Name.IsValid())
	{
		uint64_t key = (static_cast<uint64_t>(entry.Name.Value) << 8) | static_cast<uint8_t>(entry.Type);
		TypedNameIndex.erase(key);
	}

	for (auto sit = SlotIndex.begin(); sit != SlotIndex.end();)
	{
		if (sit->second == id)
			sit = SlotIndex.erase(sit);
		else
			++sit;
	}

	Entries.erase(it);
}

std::string AssetRegistry::ResolvePathByTNameAndType(TnxName name, AssetType type) const
{
	const AssetEntry* e = FindByTNameAndType(name, type);
	return e ? ResolvePath(*e) : std::string{};
}

const AssetEntry* AssetRegistry::FindByTNameAndType(TnxName name, AssetType type) const
{
	uint64_t key = (static_cast<uint64_t>(name.Value) << 8) | static_cast<uint8_t>(type);
	auto it      = TypedNameIndex.find(key);
	return it != TypedNameIndex.end() ? Find(it->second) : nullptr;
}

std::vector<const AssetEntry*> AssetRegistry::GetAssetsByName(TnxName name) const
{
	std::vector<const AssetEntry*> results;
	for (AssetType t : { AssetType::Mesh, AssetType::Skeleton, AssetType::Animation,
			 AssetType::Audio, AssetType::Material, AssetType::Texture,
			 AssetType::Level, AssetType::Prefab, AssetType::DataAsset })
	{
		if (auto* e = FindByTNameAndType(name, t)) results.push_back(e);
	}
	return results;
}

bool AssetRegistry::IsLoaded(const AssetID& id) const
{
	const AssetEntry* e = Find(id);
	return e && HasFlag(e->State, RuntimeFlags::Loaded);
}

bool AssetRegistry::IsStreaming(const AssetID& id) const
{
	const AssetEntry* e = Find(id);
	return e && HasFlag(e->State, RuntimeFlags::Streaming);
}

bool AssetRegistry::IsPinned(const AssetID& id) const
{
	const AssetEntry* e = Find(id);
	return e && e->PinCount > 0;
}

void AssetRegistry::Pin(const AssetID& id)
{
	AssetEntry* e = FindMutable(id);
	if (e) ++e->PinCount;
}

void AssetRegistry::Unpin(const AssetID& id)
{
	AssetEntry* e = FindMutable(id);
	if (e && e->PinCount > 0) --e->PinCount;
}

void AssetRegistry::RegisterLoader(AssetType type, void (*fn)(void*, AssetID), void* ctx)
{
	uint8_t idx      = static_cast<uint8_t>(type);
	Loaders[idx].Fn  = fn;
	Loaders[idx].Ctx = ctx;
}

void AssetRegistry::RegisterUploadChecker(bool (*fn)(void*), void* ctx)
{
	if (UploadCheckerCount < kMaxUploadCheckers)
		UploadCheckers[UploadCheckerCount++] = { fn, ctx };
}

bool AssetRegistry::IsUploadComplete() const
{
	for (uint8_t i = 0; i < UploadCheckerCount; ++i)
		if (!UploadCheckers[i].Fn(UploadCheckers[i].Ctx)) return false;
	return true;
}

void AssetRegistry::Checkout(const AssetID& id,
	Callback<void, uint32_t> onLoaded,
	Callback<void> onEvicted)
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

	// Demand-load: trigger loader if asset is not yet loaded.
	if (!HasFlag(e->State, RuntimeFlags::Loaded))
	{
		uint8_t typeIdx = static_cast<uint8_t>(e->Type);
		if (Loaders[typeIdx].Fn)
			Loaders[typeIdx].Fn(Loaders[typeIdx].Ctx, id);
	}
}

void AssetRegistry::Checkin(const AssetID& id, void* bindCtx)
{
	AssetEntry* e = FindMutable(id);
	if (!e) return;

	e->OnLoaded.UnbindByContext(bindCtx);
	e->OnEvicted.UnbindByContext(bindCtx);

	if (e->PinCount > 0) --e->PinCount;
}

void AssetRegistry::CheckinBySlot(AssetType type, uint32_t slot, void* bindCtx)
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

void AssetRegistry::RegisterSlot(AssetType type, uint32_t slot, const AssetID& id)
{
	uint64_t key   = (static_cast<uint64_t>(type) << 32) | slot;
	SlotIndex[key] = id;
}

void AssetRegistry::UnregisterSlot(AssetType type, uint32_t slot)
{
	uint64_t key = (static_cast<uint64_t>(type) << 32) | slot;
	SlotIndex.erase(key);
}

void AssetRegistry::DrainPendingCheckouts()
{
	auto& pending = PendingList();
	for (const PendingCheckout& pc : pending)
		Checkout(pc.ID, pc.OnLoaded, pc.OnEvicted);
	pending.clear();
}

void AssetRegistry::Evict(const AssetID& id)
{
	AssetEntry* e = FindMutable(id);
	if (e && e->PinCount == 0 && e->Data)
	{
		uint32_t slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->Data));
		UnregisterSlot(e->Type, slot);
		e->OnEvicted();
		e->OnLoaded.Reset();
		e->Data    = nullptr;
		e->CPUData = nullptr;
		e->State   = static_cast<RuntimeFlags>(
			static_cast<uint8_t>(e->State) & ~static_cast<uint8_t>(RuntimeFlags::Loaded));
	}
}

void AssetRegistry::EvictUnpinned()
{
	for (auto& [uuid, entry] : Entries)
	{
		if (entry.PinCount == 0 && entry.Data)
		{
			uint32_t slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(entry.Data));
			UnregisterSlot(entry.Type, slot);
			entry.OnEvicted();
			entry.OnLoaded.Reset();
			entry.Data    = nullptr;
			entry.CPUData = nullptr;
			entry.State   = static_cast<RuntimeFlags>(
				static_cast<uint8_t>(entry.State) & ~static_cast<uint8_t>(RuntimeFlags::Loaded));
		}
	}
}

void AssetRegistry::AddAlias(const AssetID& from, const AssetID& to)
{
	AliasTable[from] = to;
}

void AssetRegistry::Clear()
{
	Entries.clear();
	AliasTable.clear();
	TypedNameIndex.clear();
	SlotIndex.clear();
}

std::vector<PendingCheckout>& AssetRegistry::PendingList()
{
	static thread_local std::vector<PendingCheckout> tl_Pending;
	return tl_Pending;
}

std::string AssetRegistry::ResolvePath(const AssetEntry& entry) const
{
	if (entry.Path.empty() || ContentRoot.empty()) return {};
	return ContentRoot + "/" + entry.Path;
}

std::string AssetRegistry::ResolvePath(const AssetID& id) const
{
	const AssetEntry* e = Find(id);
	return e ? ResolvePath(*e) : std::string{};
}

void AssetRegistry::TriggerLoad(const AssetID& id)
{
	AssetEntry* e = FindMutable(id);
	if (!e || HasFlag(e->State, RuntimeFlags::Loaded)) return;
	uint8_t typeIdx = static_cast<uint8_t>(e->Type);
	if (Loaders[typeIdx].Fn)
		Loaders[typeIdx].Fn(Loaders[typeIdx].Ctx, id);
}

void AssetRegistry::RegisterPendingCheckout(TnxName name, AssetType type, AssetLoad onLoaded, AssetEvict onEvicted)
{
	const AssetEntry* entry = Get().FindByTNameAndType(name, type);
	if (!entry)
	{
		LOG_ENG_WARN_F("AssetRegistry::RegisterPendingCheckout - asset '%s' (type: %s) not found in registry",
			name.GetStr(), AssetTypeName(type));
		return;
	}
	PendingList().push_back({ onLoaded, onEvicted, entry->ID });
}
