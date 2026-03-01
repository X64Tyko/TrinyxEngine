#pragma once
#include <vector>
#include "Types.h"

// Field metadata for SoA decomposition
struct FieldMeta
{
	size_t Size;           // sizeof(field) - e.g., sizeof(float) = 4
	size_t Alignment;      // alignof(field) - e.g., alignof(float) = 4
	size_t OffsetInStruct; // offsetof(Component, field) - for validation
	size_t OffsetInChunk;  // Where this field array starts in the chunk (computed by BuildLayout)
	const char* Name;      // Field name for debugging
};

// Enhanced component metadata with field decomposition support
struct ComponentMetaEx
{
	ComponentTypeID TypeID;        // Numeric ID (0-255) for this component type
	size_t Size;                   // sizeof(Component) - total struct size
	size_t Alignment;              // alignof(Component)
	size_t OffsetInChunk;          // Where this component's data starts in the chunk
	bool IsFieldDecomposed;        // True if stored as field arrays (SoA)
	bool IsTemporal;               // True if this component should live in TemporalComponentCache
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
	void RegisterFields(ComponentTypeID typeID, std::vector<FieldMeta>&& fields, bool bIsTemporal)
	{
		ComponentMetaEx& meta = ComponentData[typeID];
		if (meta.Fields.size() != 0) return;

		meta.TypeID            = typeID;
		meta.IsFieldDecomposed = true;
		meta.IsTemporal        = bIsTemporal;
		meta.Fields            = std::move(fields);
		for (const auto& field : meta.Fields) meta.Size += field.Size;
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

	const std::unordered_map<ComponentTypeID, ComponentMetaEx>& GetAllComponents() const { return ComponentData; }
	const ComponentMetaEx& GetComponentMeta(ComponentTypeID typeID) const { return ComponentData.at(typeID); }

private:
	std::unordered_map<ComponentTypeID, ComponentMetaEx> ComponentData;
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