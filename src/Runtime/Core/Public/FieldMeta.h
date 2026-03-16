#pragma once
#include <string_view>
#include <vector>
#include "MemoryDefines.h"
#include "Types.h"

// Runtime type tag for editor display and serialization.
enum class FieldValueType : uint8_t
{
	Unknown = 0,
	Float32,
	Float64,
	Int32,
	Uint32,
	Int64,
	Uint64,
	Fixed32,
};

// Field metadata for SoA decomposition
struct FieldMeta
{
	size_t Size;           // sizeof(field) - e.g., sizeof(float) = 4
	size_t Alignment;      // alignof(field) - e.g., alignof(float) = 4
	size_t OffsetInStruct; // offsetof(Component, field) - for validation
	size_t OffsetInChunk;  // Where this field array starts in the chunk (computed by BuildLayout)
	const char* Name;      // Field name for debugging
	FieldValueType ValueType = FieldValueType::Unknown;
};

// Enhanced component metadata with field decomposition support
struct ComponentMetaEx
{
	ComponentTypeID TypeID;        // Numeric ID (0-255) for this component type
	const char* Name = nullptr;    // Stringified component type name (from registration macro)
	size_t Size;                   // sizeof(Component) - total struct size
	size_t Alignment;              // alignof(Component)
	size_t OffsetInChunk;          // Where this component's data starts in the chunk
	bool IsFieldDecomposed;        // True if stored as field arrays (SoA)
	CacheTier TemporalTier;        // Which slab this component lives in (None = cold/chunk only)
	uint8_t CacheSlotIndex = 0xFF; // Per-slab slot index — set via StaticTemporalIndex() at registration.
	// 0xFF = unassigned (cold components with TemporalTier::None).
	std::vector<FieldMeta> Fields; // Field layout if decomposed
};

// Component field registry - static storage for field decomposition info
// TODO: Should this stay separate or should it be rolled into the MetaRegistry?
// Also need to double check how the .data and compile times are looking with all this "Reflection"
class ComponentFieldRegistry
{
public:
	static ComponentFieldRegistry& Get()
	{
		static ComponentFieldRegistry instance;
		return instance;
	}

	// Register field decomposition for a component type
	void RegisterFields(ComponentTypeID typeID, const char* name, std::vector<FieldMeta>&& fields, CacheTier inTier, uint8_t slot)
	{
		ComponentMetaEx& meta = ComponentData[typeID];
		if (meta.Fields.size() != 0) return;

		meta.TypeID            = typeID;
		meta.Name              = name;
		meta.IsFieldDecomposed = true;
		meta.TemporalTier      = inTier;
		meta.Fields            = std::move(fields);
		meta.CacheSlotIndex    = slot;
		for (const auto& field : meta.Fields) meta.Size += field.Size;

		NameToComponentID[std::string_view(name)] = typeID;
	}

	// Get field layout for a component
	[[nodiscard]] const std::vector<FieldMeta>* GetFields(ComponentTypeID typeID) const
	{
		auto it = ComponentData.find(typeID);
		return it != ComponentData.end() ? &it->second.Fields : nullptr;
	}

	// Check if component has field decomposition
	[[nodiscard]] bool IsDecomposed(ComponentTypeID typeID) const
	{
		return ComponentData.contains(typeID);
	}

	// Get field count
	[[nodiscard]] size_t GetFieldCount(ComponentTypeID typeID) const
	{
		auto it = ComponentData.find(typeID);
		return it != ComponentData.end() ? it->second.Fields.size() : 0;
	}

	[[nodiscard]] CacheTier GetTemporalTier(ComponentTypeID typeID) const
	{
		auto it = ComponentData.find(typeID);
		return it != ComponentData.end() ? it->second.TemporalTier : CacheTier::None;
	}

	// Stores the slab slot index assigned by StaticTemporalIndex() at entity registration time.
	// Idempotent — only writes if still 0xFF (unassigned).
	void SetCacheSlotIndex(ComponentTypeID typeID, uint8_t slot)
	{
		ComponentMetaEx& meta = ComponentData[typeID];
		if (meta.CacheSlotIndex == 0xFF) meta.CacheSlotIndex = slot;
	}

	const std::unordered_map<ComponentTypeID, ComponentMetaEx>& GetAllComponents() const { return ComponentData; }
	const ComponentMetaEx& GetComponentMeta(ComponentTypeID typeID) const { return ComponentData.at(typeID); }
	uint8_t GetCacheSlotIndex(ComponentTypeID typeID) const { return ComponentData.at(typeID).CacheSlotIndex; }

	// Reverse lookup: component name → TypeID. Returns 0 if not found.
	[[nodiscard]] ComponentTypeID GetComponentByName(std::string_view name) const
	{
		auto it = NameToComponentID.find(name);
		return it != NameToComponentID.end() ? it->second : 0;
	}

private:
	std::unordered_map<ComponentTypeID, ComponentMetaEx> ComponentData;
	std::unordered_map<std::string_view, ComponentTypeID> NameToComponentID;
};

// Hash specialization for std::unordered_set
namespace std
{
	template <>
	struct hash<ComponentMetaEx>
	{
		size_t operator()(const ComponentMetaEx& Meta) const noexcept
		{
			return hash<uint64_t>()(Meta.TypeID);
		}
	};
}