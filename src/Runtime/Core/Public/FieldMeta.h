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
