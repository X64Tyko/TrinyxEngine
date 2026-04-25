#include "ReflectionRegistry.h"

#include "AssetRegistry.h"
#include "AssetTypes.h"
#include "FieldMeta.h"
#include "Logger.h"
#include "NetTypes.h"
#include "Soul.h"

#include <cassert>
#include <cstring>

// ---------------------------------------------------------------------------
// Component field registration
// ---------------------------------------------------------------------------

void ReflectionRegistry::RegisterFields(ComponentTypeID typeID, const char* name,
										std::vector<FieldMeta>&& fields, CacheTier tier, uint8_t slot)
{
	ComponentMetaEx& meta = ComponentData[typeID];
	if (!meta.Fields.empty()) return; // Already registered

	meta.TypeID            = typeID;
	meta.Name              = name;
	meta.IsFieldDecomposed = true;
	meta.TemporalTier      = tier;
	meta.Fields            = std::move(fields);
	meta.CacheSlotIndex    = slot;
	for (const auto& field : meta.Fields) meta.Size += field.Size;

	NameToComponentID[std::string(name)] = typeID;
}

// ---------------------------------------------------------------------------
// Entity accessors
// ---------------------------------------------------------------------------

ClassID ReflectionRegistry::GetEntityByName(std::string_view name) const
{
	auto it = NameToClassID.find(std::string(name));
	return it != NameToClassID.end() ? it->second : 0;
}

// ---------------------------------------------------------------------------
// Component accessors
// ---------------------------------------------------------------------------

const std::vector<FieldMeta>* ReflectionRegistry::GetFields(ComponentTypeID typeID) const
{
	auto it = ComponentData.find(typeID);
	return it != ComponentData.end() ? &it->second.Fields : nullptr;
}

bool ReflectionRegistry::IsDecomposed(ComponentTypeID typeID) const
{
	return ComponentData.contains(typeID);
}

size_t ReflectionRegistry::GetFieldCount(ComponentTypeID typeID) const
{
	auto it = ComponentData.find(typeID);
	return it != ComponentData.end() ? it->second.Fields.size() : 0;
}

CacheTier ReflectionRegistry::GetTemporalTier(ComponentTypeID typeID) const
{
	auto it = ComponentData.find(typeID);
	return it != ComponentData.end() ? it->second.TemporalTier : CacheTier::None;
}

uint8_t ReflectionRegistry::GetCacheSlotIndex(ComponentTypeID typeID) const
{
	return ComponentData.at(typeID).CacheSlotIndex;
}

const ComponentMetaEx& ReflectionRegistry::GetComponentMeta(ComponentTypeID typeID) const
{
	return ComponentData.at(typeID);
}

ComponentTypeID ReflectionRegistry::GetComponentByName(std::string_view name) const
{
	auto it = NameToComponentID.find(std::string(name));
	return it != NameToComponentID.end() ? it->second : 0;
}

void ReflectionRegistry::SetCacheSlotIndex(ComponentTypeID typeID, uint8_t slot)
{
	ComponentMetaEx& meta = ComponentData[typeID];
	if (meta.CacheSlotIndex == 0xFF) meta.CacheSlotIndex = slot;
}

// ---------------------------------------------------------------------------
// FlowState / GameMode registration
// ---------------------------------------------------------------------------

void ReflectionRegistry::RegisterState(const char* name, StateFactory factory)
{
	for (const auto& entry : RegisteredStates)
	{
		if (strcmp(entry.Name, name) == 0) return;
	}
	int64_t uuid = UUIDFromName(name);
	size_t idx   = RegisteredStates.size();
	RegisteredStates.push_back({name, uuid, std::move(factory)});
	StateUUIDIndex[uuid] = idx;
}

void ReflectionRegistry::RegisterMode(const char* name, ModeFactory factory)
{
	for (const auto& entry : RegisteredModes)
	{
		if (strcmp(entry.Name, name) == 0) return;
	}
	int64_t uuid = UUIDFromName(name);
	size_t idx   = RegisteredModes.size();
	RegisteredModes.push_back({name, uuid, std::move(factory)});
	ModeUUIDIndex[uuid] = idx;
}

ReflectionRegistry::StateFactory ReflectionRegistry::FindStateFactory(const char* name) const
{
	for (const auto& entry : RegisteredStates)
	{
		if (strcmp(entry.Name, name) == 0) return entry.Factory;
	}
	return nullptr;
}

ReflectionRegistry::ModeFactory ReflectionRegistry::FindModeFactory(const char* name) const
{
	for (const auto& entry : RegisteredModes)
	{
		if (strcmp(entry.Name, name) == 0) return entry.Factory;
	}
	return nullptr;
}

ReflectionRegistry::StateFactory ReflectionRegistry::FindStateByUUID(int64_t uuid) const
{
	auto it = StateUUIDIndex.find(uuid);
	return it != StateUUIDIndex.end() ? RegisteredStates[it->second].Factory : nullptr;
}

ReflectionRegistry::ModeFactory ReflectionRegistry::FindModeByUUID(int64_t uuid) const
{
	auto it = ModeUUIDIndex.find(uuid);
	return it != ModeUUIDIndex.end() ? RegisteredModes[it->second].Factory : nullptr;
}

void ReflectionRegistry::RegisterMixin(const char* name, uint8_t baseTypeID, bool isUserDefined)
{
	// Collision check — asserts rather than silently dropping to catch misconfigured ID bands early.
	assert(MixinIDIndex.find(baseTypeID) == MixinIDIndex.end() &&
		"ModeMixin ID collision: two mixins claim the same BaseTypeID. "
		"Check TNX_REGISTER_MODEMIX order or engine mixin ID bands.");

	int64_t uuid = UUIDFromName(name);
	size_t idx   = RegisteredMixins.size();
	RegisteredMixins.push_back({name, uuid, baseTypeID, isUserDefined});
	MixinIDIndex[baseTypeID] = idx;
}

const ReflectionRegistry::MixinEntry* ReflectionRegistry::FindMixin(const char* name) const
{
	for (const auto& entry : RegisteredMixins) if (strcmp(entry.Name, name) == 0) return &entry;
	return nullptr;
}

const ReflectionRegistry::MixinEntry* ReflectionRegistry::FindMixinByID(uint8_t baseTypeID) const
{
	auto it = MixinIDIndex.find(baseTypeID);
	return it != MixinIDIndex.end() ? &RegisteredMixins[it->second] : nullptr;
}

int64_t ReflectionRegistry::UUIDFromName(const char* name)
{
	// FNV-1a 32-bit hash
	uint32_t hash = 2166136261u;
	for (const char* p = name; *p; ++p) hash = (hash ^ static_cast<uint8_t>(*p)) * 16777619u;
	return static_cast<int64_t>(hash) << 8;
}

uint16_t ReflectionRegistry::ConstructTypeHashFromName(const char* name)
{
	// FNV-1a 32-bit folded to 16 bits via XOR-folding
	uint32_t hash = 2166136261u;
	for (const char* p = name; *p; ++p) hash = (hash ^ static_cast<uint8_t>(*p)) * 16777619u;
	return static_cast<uint16_t>(hash ^ (hash >> 16));
}

void ReflectionRegistry::PublishToAssetRegistry() const
{
	AssetRegistry& registry = AssetRegistry::Get();

	for (const auto& entry : RegisteredStates)
	{
		AssetID id = AssetID::Create(entry.UUID, AssetType::FlowState);
		registry.Register(id, entry.Name, "", AssetType::FlowState);
	}

	for (const auto& entry : RegisteredModes)
	{
		AssetID id = AssetID::Create(entry.UUID, AssetType::GameMode);
		registry.Register(id, entry.Name, "", AssetType::GameMode);
	}

	for (const auto& [name, classID] : NameToClassID)
	{
		int64_t uuid = UUIDFromName(name.c_str());
		AssetID id   = AssetID::Create(uuid, AssetType::EntityType);
		registry.Register(id, name, "", AssetType::EntityType);
	}
}

// ---------------------------------------------------------------------------
// Replicated Construct factory table
// ---------------------------------------------------------------------------

void ReflectionRegistry::RegisterConstruct(const char* name, uint16_t typeHash, ConstructClientFactory factory)
{
	for (const auto& entry : RegisteredConstructs)
	{
		if (strcmp(entry.Name, name) == 0) return; // already registered
	}
	RegisteredConstructs.push_back({name, typeHash, factory});
}

ReflectionRegistry::ConstructClientFactory ReflectionRegistry::FindConstructClientFactory(uint16_t typeHash) const
{
	for (const auto& entry : RegisteredConstructs)
	{
		if (entry.TypeHash == typeHash) return entry.ClientFactory;
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// SoulRPC dispatch table
// ---------------------------------------------------------------------------

void ReflectionRegistry::RegisterServerRPC(uint16_t methodID, uint16_t paramSize, SoulRPCHandler handler)
{
	if (methodID >= ServerRPCTable.size()) ServerRPCTable.resize(methodID + 1);
	ServerRPCTable[methodID] = {paramSize, handler};
}

void ReflectionRegistry::RegisterClientRPC(uint16_t methodID, uint16_t paramSize, SoulRPCHandler handler)
{
	if (methodID >= ClientRPCTable.size()) ClientRPCTable.resize(methodID + 1);
	ClientRPCTable[methodID] = {paramSize, handler};
}

bool ReflectionRegistry::DispatchServerRPC(Soul* soul, const RPCContext& ctx,
										   const RPCHeader& hdr, const uint8_t* params) const
{
	if (hdr.MethodID >= ServerRPCTable.size())
	{
		LOG_ENG_WARN_F("[RPC] DispatchServerRPC: unknown MethodID %u", hdr.MethodID);
		return false;
	}
	const RPCEntry& entry = ServerRPCTable[hdr.MethodID];
	if (!entry.Handler)
	{
		LOG_ENG_WARN_F("[RPC] DispatchServerRPC: no handler for MethodID %u", hdr.MethodID);
		return false;
	}
	if (entry.ParamSize != hdr.ParamSize)
	{
		LOG_ENG_WARN_F("[RPC] DispatchServerRPC: ParamSize mismatch for MethodID %u (expected %u, got %u)",
					   hdr.MethodID, entry.ParamSize, hdr.ParamSize);
		return false;
	}
	entry.Handler(soul, ctx, params);
	return true;
}

bool ReflectionRegistry::DispatchClientRPC(Soul* soul, const RPCContext& ctx,
										   const RPCHeader& hdr, const uint8_t* params) const
{
	if (hdr.MethodID >= ClientRPCTable.size())
	{
		LOG_ENG_WARN_F("[RPC] DispatchClientRPC: unknown MethodID %u", hdr.MethodID);
		return false;
	}
	const RPCEntry& entry = ClientRPCTable[hdr.MethodID];
	if (!entry.Handler)
	{
		LOG_ENG_WARN_F("[RPC] DispatchClientRPC: no handler for MethodID %u", hdr.MethodID);
		return false;
	}
	if (entry.ParamSize != hdr.ParamSize)
	{
		LOG_ENG_WARN_F("[RPC] DispatchClientRPC: ParamSize mismatch for MethodID %u (expected %u, got %u)",
					   hdr.MethodID, entry.ParamSize, hdr.ParamSize);
		return false;
	}
	entry.Handler(soul, ctx, params);
	return true;
}
