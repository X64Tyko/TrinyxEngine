#pragma once
#include "PagedMap.h"

// Disable MSVC warning for anonymous structs in unions (C++11 standard feature)
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201) // nonstandard extension used: nameless struct/union
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif


static constexpr size_t NetOwnerID_Bits  = 8;
static constexpr size_t Generation_Bits  = 16;
static constexpr size_t TypeKey_Bits     = 16;
static constexpr size_t UniqueIndex_Bits = 24;

// Lives in chunk storage, easily iterable, global index is derived, not stored.
union EntitySlotMeta
{
	uint32_t Value;

	struct
	{
		uint32_t Generation : Generation_Bits; // generation still lives with the entity, usable by logic and renderer
		uint32_t NetOwnerID : NetOwnerID_Bits; // still 256 possible owner IDs held per entity for comparison
		uint32_t reserved   : 7;               // genuinely not sure what else they'd need yet
		uint32_t ValidBit   : 1;               // If the entity is actually alive (bit 31 matches the existing precedent with Flags)
	};

	// Required interface (for swappability) Don't assume sizes, as they can be customized
	//uint32_t GetIndex() const { return static_cast<uint32_t>(Index); }
	uint32_t GetGeneration() const { return Generation; }
	uint32_t GetOwnerID() const { return NetOwnerID; }

	bool IsValid() const { return ValidBit == 0x1; }

	static EntitySlotMeta Invalid()
	{
		EntitySlotMeta Id;
		Id.Value = 0;
		return Id;
	}

	// Comparison operators
	bool operator==(const EntitySlotMeta& other) const { return Value == other.Value; }
	bool operator!=(const EntitySlotMeta& other) const { return Value != other.Value; }

	// Network/ownership helpers
	bool IsServer() const { return NetOwnerID == 0; }
	bool IsLocal(uint8_t localClientID) const { return NetOwnerID == localClientID; }
};

static_assert(sizeof(EntitySlotMeta) == 4, "EntityIDNew must be 4 bytes");

// Network ID, maps the owner and an index into a network array to easily look up entities.
union EntityNetHandle
{
	uint32_t Value;

	struct
	{
		uint32_t NetOwnerID : NetOwnerID_Bits;  // With this in the NetID we have an entity index and an owner, this means that on the server we have a single array of net entities, each owners local entities live contiguously. 0 is global
		uint32_t NetIndex   : UniqueIndex_Bits; // Net ID size matches the EntityHandle Index. This could probably be smaller to allow packing some other data specifically for networking as 16M networked entities is a lot.
	};

	uint32_t GetOwnerID() const { return NetOwnerID; }
	uint32_t GetHandleIndex() const { return NetIndex; }

	// Network/ownership helpers
	bool IsServer() const { return NetOwnerID == 0; }
	bool IsLocal(uint8_t localClientID) const { return NetOwnerID == localClientID; }
};

// Can be sent with an EntityNetHandle and functions similarly to a GlobalHandle. Allows to send a handle over the wire so
// we can build entities nearly lookup free on the receiving side.
union EntityNetManifest
{
	uint32_t Value;

	struct
	{
		uint32_t ClassType : TypeKey_Bits; // Class type so that creation requests can come directly from a handle and allow handle reuse and mutation by the registry.
		uint32_t NetFlags  : 8;            // Various flags for net manifest
		uint32_t reserved  : 8;            // reserved for later
	};
	
	// Signifies that this manifest is for a predicted entity creation
	bool IsPredictedCreation() const { return NetFlags & 0x1; }
	// Signifies that following this uint32 is an EntityNetHandle this manifest is intended to modify
	bool HasEntityHandle() const { return NetFlags & 0x2; }
	bool PredictionResult() const { return NetFlags & 0x12; }
};

static_assert(sizeof(EntityNetHandle) == 4, "NetID must be 4 bytes");

using EntityCacheHandle = uint32_t;
