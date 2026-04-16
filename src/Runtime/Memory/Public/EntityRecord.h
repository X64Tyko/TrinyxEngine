#pragma once
#include <cstdint>
#include <type_traits>

#include "RegistryTypes.h"
#include "Events.h"
#include "PagedMap.h"

class Registry;
class Archetype;
struct Chunk;

DEFINE_FIXED_MULTICALLBACK(EntityCacheSlotChange, 64, uint32_t, uint32_t)

// Local handle — held by OOP code (Constructs, Views) to reference ECS entities.
// Always local; never crosses the network. Self-validating: embeds the entity's
// generation at creation time so stale handles are caught without a separate Ref type.
//
// Ownership:   not stored here — fetch from EntityRecord.GetOwnerID() if needed.
union EntityHandle
{
protected:
	uint64_t Value;

	struct
	{
		uint64_t HandleIndex : UniqueIndex_Bits; // index into LocalToRecord
		uint64_t ClassType   : TypeKey_Bits;     // entity class — enables handle-driven creation/mutation
		uint64_t Generation  : Generation_Bits;  // generation at creation — compare against EntityInfo for ABA detection
		uint64_t reserved    : 8;
	};

public:
	EntityHandle()
		: Value(0)
	{
	}

	// Read-only public API
	uint32_t GetHandleIndex() const { return HandleIndex; }
	uint32_t GetTypeID() const { return ClassType; }
	uint16_t GetGeneration() const { return Generation; }
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
	constexpr GlobalEntityHandle()
		: Value(0)
	{
	}

	GlobalEntityHandle(uint32_t index, uint32_t generation, uint32_t prefabIndex)
		: Index(index)
		, Generation(generation)
		, PrefabIndex(prefabIndex)
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
	explicit constexpr GlobalEntityHandle(const uint64_t value)
		: Value(value)
	{
	}
};

static_assert(sizeof(GlobalEntityHandle) == 8, "GlobalEntityHandle must be 8 bytes");

// A global array of entity records holds the entire entity list of active entities, Their index in the record array is their GlobalIndex, from which we can find
// Their location in cache, network, or Handle lists.
struct EntityRecord
{
	//uint32_t ArchiveKey  = 0; // This entities key in the Entity Archive
	// Set up by the systems responsible for ECS<->OOP and ECS<->Network allocation
	EntityNetHandle NetworkID{}; // deterministic network replicated entity ID
	EntityHandle LHandle;        // Local handle — index into LocalToRecord (OOP land)

	EntityCacheHandle CacheEntityIndex{}; // Cache slab index
	EntitySlotMeta EntityInfo{};

	uint32_t ArchIndex  = 0; // Flat index across all chunks in the archetype
	uint32_t ChunkIndex = 0; // Which chunk in the archetype's Chunks[] array
	uint32_t LocalIndex = 0; // Index within the chunk (0 .. EntitiesPerChunk-1)

	Archetype* Arch    = nullptr; // Which archetype this entity belongs to
	Chunk* TargetChunk = nullptr; // Which chunk within that archetype

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
	friend class ReplicationSystem;

	// Private constructor - only Registry can create
	EntityArchive() = default;

	// Lookup methods - return GlobalEntityHandle (only accessible to Registry via friendship)
	GlobalEntityHandle LookupGlobalHandle(const EntityHandle& handle) const { return LocalToRecord[handle.GetHandleIndex()]; }
	GlobalEntityHandle LookupGlobalHandle(const EntityNetHandle& handle) const { return NetToRecord[handle.GetHandleIndex()]; }
	GlobalEntityHandle LookupGlobalHandle(EntityCacheHandle handle) const { return CacheToRecord[handle]; }

	// Private GetRecord - mutable access for Registry only
	template <typename T>
	EntityRecord* GetRecordPtr(T handle) requires (std::same_as<std::remove_cvref_t<T>, EntityHandle> || std::same_as<std::remove_cvref_t<T>, EntityNetHandle> || std::same_as<std::remove_cvref_t<T>, EntityCacheHandle>)
	{
		if (!IsHandleValid(handle)) return nullptr;

		const GlobalEntityHandle& gHandle = LookupGlobalHandle(handle);
		return Records[gHandle.GetIndex()];
	}

	// Storage - fully private
	PagedMap<1 << UniqueIndex_Bits, EntityRecord> Records{};
	PagedMap<1 << UniqueIndex_Bits, GlobalEntityHandle> NetToRecord{};
	PagedMap<1 << UniqueIndex_Bits, GlobalEntityHandle> CacheToRecord{};
	PagedMap<1 << UniqueIndex_Bits, GlobalEntityHandle> LocalToRecord{};

public:
	// Public read-only API - returns record by value (can't modify internal state)
	template <typename T>
	EntityRecord GetRecord(T handle) const requires (std::same_as<std::remove_cvref_t<T>, EntityHandle> || std::same_as<std::remove_cvref_t<T>, EntityNetHandle> || std::same_as<std::remove_cvref_t<T>, EntityCacheHandle>)
	{
		const GlobalEntityHandle& gHandle = LookupGlobalHandle(handle);
		return Records[gHandle.GetIndex()];
	}

	// Public validation - read-only check
	bool IsHandleValid(const EntityHandle& handle) const
	{
		const GlobalEntityHandle& gHandle = LookupGlobalHandle(handle);
		return handle.GetGeneration() == Records[gHandle.GetIndex()].EntityInfo.GetGeneration();
	}

	bool IsHandleValid(const EntityNetHandle& handle) const
	{
		const GlobalEntityHandle& gHandle = LookupGlobalHandle(handle);
		return gHandle.GetGeneration() == Records[gHandle.GetIndex()].EntityInfo.GetGeneration();
	}

	bool IsHandleValid(EntityCacheHandle handle) const
	{
		const GlobalEntityHandle& gHandle = LookupGlobalHandle(handle);
		return gHandle.GetGeneration() == Records[gHandle.GetIndex()].EntityInfo.GetGeneration();
	}

	// EntityRef validation — skips GlobalEntityHandle generation, uses the generation
	// embedded in the ref directly. One fewer map lookup vs IsHandleValid(EntityNetHandle).
	// Use this for all client→server RPC and net-boundary handle checks.
	bool IsHandleValid(const EntityRef& ref) const
	{
		const GlobalEntityHandle& gHandle = LookupGlobalHandle(ref.Handle);
		return ref.Generation == Records[gHandle.GetIndex()].EntityInfo.GetGeneration();
	}
};
