#pragma once
#include <cstdint>

#include "RegistryTypes.h"
#include "Events.h"
#include "PagedMap.h"

class Registry;
class Archetype;
struct Chunk;

DEFINE_FIXED_MULTICALLBACK(EntityCacheSlotChange, 6, uint32_t, uint32_t)

// Handle ID, maps the owner and an index into a Handle array to easily look up entities.
// Handles are held in OOP land by Constructs to reference various entities.
// A handle is functionally identical to a NetID, but it's worth making a distinction when referencing them in code so they aren't confused. A NetworkIndex doesn't necessarily match a Handle Index
union EntityHandle
{
protected:
	uint64_t Value;

	struct
	{
		uint64_t NetOwnerID  : NetOwnerID_Bits;  // With this in the NetID we have an entity index and an owner, this means that on the server we have a single array of net entities, each owners local entities live contiguously. 0 is global
		uint64_t HandleIndex : UniqueIndex_Bits; // Net ID size matches the EntityHandle Index. This could probably be smaller to allow packing some other data specifically for networking as 16M networked entities is a lot.
		uint64_t ClassType   : TypeKey_Bits;     // Class type so that creation requests can come directly from a handle and allow handle reuse and mutation by the registry.
		uint64_t reserved    : 8;                // Likely good for flags, like if the existing entity should be destroyed when this handle is recycled or validation
	};

public:
	EntityHandle()
		: Value(0)
	{
	}

	// Read-only public API
	uint32_t GetOwnerID() const { return NetOwnerID; }
	uint32_t GetHandleIndex() const { return HandleIndex; }
	uint32_t GetTypeID() const { return ClassType; }
	bool IsValid() const { return Value != 0; }

private:
	friend class Registry;
	friend struct EntityArchive;

	// Private value constructor - only Registry can create handles with specific values
	explicit EntityHandle(const uint64_t value)
		: Value(value)
	{
	}
};

static_assert(sizeof(EntityHandle) == 8, "EntityHandle must be 8 bytes");

// Internally used handle, used for registry<->registry communication Outside handles point to one of these
union GlobalEntityHandle
{
protected:
	uint64_t Value;

	struct
	{
		uint64_t Index       : UniqueIndex_Bits; // The index into the Entity Archive
		uint64_t Generation  : Generation_Bits;  // used to compare against the slot generation, mismatch means our handle is old
		uint64_t PrefabIndex : 16;               // index into the AssetRegistry loaded prefabs, Since these are only mutable in the registry we can track reference counts and safely unload prefabs unless pinned by users.
		uint64_t reserved    : 8;                // reserved for internal use later.
	};

public:
	GlobalEntityHandle()
		: Value(0)
	{
	}

	// Read-only public API
	uint32_t GetIndex() const { return Index; }
	uint32_t GetGeneration() const { return Generation; }
	uint16_t GetPrefabIndex() const { return PrefabIndex; }

private:
	friend class Registry;
	friend struct EntityArchive;

	// Private value constructor - only Registry can create handles with specific values
	explicit GlobalEntityHandle(const uint64_t value)
		: Value(value)
	{
	}
};

static_assert(sizeof(GlobalEntityHandle) == 8, "GlobalEntityHandle must be 8 bytes");

// A global array of entity records holds the entire entity list of active entities, Their index in the record array is their GlobalIndex, from which we can find
// Their location in cache, network, or Handle lists.
struct EntityRecord
{
	EntityNetHandle NetworkID; // deterministic network replicated entity ID
	EntityHandle Handle;       // Index into the entity Handle array

	EntityCacheHandle CacheEntityIndex; // Cache slab index
	EntitySlotMeta EntityInfo;

	Archetype* Arch     = nullptr; // Which archetype this entity belongs to
	Chunk* TargetChunk  = nullptr; // Which chunk within that archetype
	uint32_t ArchIndex  = 0;       // index at the archetype level
	uint32_t ChunkIndex = 0;       // index at the chunk level

	// Callback EntityViews register to so they can update their hydrated components with the correct index
	EntityCacheSlotChange OnCacheSlotChange;

	bool IsValid() const { return Arch != nullptr && TargetChunk != nullptr && EntityInfo.IsValid(); }

	uint32_t GetGeneration() const { return EntityInfo.GetGeneration(); }
	uint32_t GetOwnerID() const { return EntityInfo.GetOwnerID(); }
};

struct EntityArchive
{
private:
	friend class Registry;

	// Private constructor - only Registry can create
	EntityArchive() = default;

	// Lookup methods - return GlobalEntityHandle (only accessible to Registry via friendship)
	GlobalEntityHandle LookupGlobalHandle(const EntityHandle& inHandle) const { return LocalToRecord[inHandle.GetHandleIndex()]; }
	GlobalEntityHandle LookupGlobalHandle(const EntityNetHandle& inHandle) const { return NetToRecord[inHandle.GetHandleIndex()]; }
	GlobalEntityHandle LookupGlobalHandle(EntityCacheHandle inHandle) const { return CacheToRecord[inHandle]; }

	// Private GetRecord - mutable access for Registry only
	template <typename T>
	EntityRecord* GetRecordPtr(T InHandle) requires (std::same_as<T, EntityHandle> || std::same_as<T, EntityNetHandle> || std::same_as<T, EntityCacheHandle>)
	{
		if (!IsHandleValid(InHandle)) return nullptr;

		const GlobalEntityHandle& GHandle = LookupGlobalHandle(InHandle);
		return &Records[GHandle.GetIndex()];
	}

	// Storage - fully private
	PagedMap<1 << UniqueIndex_Bits, EntityRecord> Records;
	PagedMap<1 << UniqueIndex_Bits, GlobalEntityHandle> NetToRecord;
	PagedMap<1 << UniqueIndex_Bits, GlobalEntityHandle> CacheToRecord;
	PagedMap<1 << UniqueIndex_Bits, GlobalEntityHandle> LocalToRecord;

public:
	// Public read-only API - returns record by value (can't modify internal state)
	template <typename T>
	EntityRecord GetRecord(T InHandle) const requires (std::same_as<T, EntityHandle> || std::same_as<T, EntityNetHandle> || std::same_as<T, EntityCacheHandle>)
	{
		const GlobalEntityHandle& GHandle = LookupGlobalHandle(InHandle);
		return Records[GHandle.GetIndex()];
	}

	// Public validation - read-only check
	template <typename T>
	bool IsHandleValid(T InHandle) const requires (std::same_as<T, EntityHandle> || std::same_as<T, EntityNetHandle> || std::same_as<T, EntityCacheHandle>)
	{
		const GlobalEntityHandle& GHandle = LookupGlobalHandle(InHandle);
		return GHandle.GetGeneration() == Records[GHandle.GetIndex()].EntityInfo.GetGeneration();
	}
};
