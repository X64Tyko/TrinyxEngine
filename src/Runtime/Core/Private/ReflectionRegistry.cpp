#include "ReflectionRegistry.h"

#include "FieldMeta.h"
#include "Logger.h"

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
// GameState / GameMode registration
// ---------------------------------------------------------------------------

void ReflectionRegistry::RegisterState(const char* name, StateFactory factory)
{
	// Check for duplicate
	for (const auto& entry : RegisteredStates)
	{
		if (strcmp(entry.Name, name) == 0) return;
	}
	RegisteredStates.push_back({name, std::move(factory)});
}

void ReflectionRegistry::RegisterMode(const char* name, ModeFactory factory)
{
	for (const auto& entry : RegisteredModes)
	{
		if (strcmp(entry.Name, name) == 0) return;
	}
	RegisteredModes.push_back({name, std::move(factory)});
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
