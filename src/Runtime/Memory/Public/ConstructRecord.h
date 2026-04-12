#pragma once
#include <cstdint>

#include "RegistryTypes.h"
#include "PagedMap.h"

class Registry;
class ReplicationSystem;

// ConstructNetManifest — packed 32-bit type descriptor sent alongside a ConstructNetHandle.
// Allows the receiver to construct the correct type without a round-trip lookup.
union ConstructNetManifest
{
	uint32_t Value;

	struct
	{
		uint32_t PrefabIndex : TypeKey_Bits; // Index into AssetRegistry loaded prefab table (not a raw AssetID)
		uint32_t NetFlags    : 8;            // Spawn/mutation flags
		uint32_t reserved    : 8;
	};

	// Signifies that this manifest is for a predicted Construct creation
	bool IsPredictedCreation() const { return NetFlags & 0x1; }
	// Signifies that following this uint32 is an EntityNetHandle this manifest is intended to modify
	bool HasEntityHandle() const { return NetFlags & 0x2; }
	bool PredictionResult() const { return NetFlags & 0x4; } // confirmed=1, rejected=0
};

// GlobalConstructHandle — internal handle used by ConstructRegistry.
// Never sent over the wire. Gameplay and net-boundary code uses ConstructRef (RegistryTypes.h).
union GlobalConstructHandle
{
protected:
	uint64_t Value;

	struct
	{
		uint64_t Index       : UniqueIndex_Bits; // Index into ConstructArchive::Records
		uint64_t Generation  : Generation_Bits;  // ABA protection — compare against ConstructRecord::Generation
		uint64_t PrefabIndex : 16;               // Index into AssetRegistry loaded prefab table (for ref counting)
		uint64_t reserved    : 8;
	};

public:
	constexpr GlobalConstructHandle()
		: Value(0)
	{
	}

	GlobalConstructHandle(uint32_t index, uint32_t generation, uint32_t prefabIndex)
		: Index(index)
		, Generation(generation)
		, PrefabIndex(prefabIndex)
	{
	}

	uint32_t GetIndex() const { return Index; }
	uint32_t GetGeneration() const { return Generation; }
	uint16_t GetPrefabIndex() const { return PrefabIndex; }
	bool IsValid() const { return Value != 0; }

private:
	friend class ConstructRegistry;

	explicit constexpr GlobalConstructHandle(uint64_t value)
		: Value(value)
	{
	}
};

static_assert(sizeof(GlobalConstructHandle) == 8, "GlobalConstructHandle must be 8 bytes");

// ConstructRecord — one entry in ConstructArchive's PagedMap.
// Constructs are singular OOP objects, not archetype/chunk residents.
// No cache slot, no local handle space — gameplay holds typed pointers directly.
struct ConstructRecord
{
	ConstructNetHandle NetworkID{}; // Wire identity (OwnerID + NetIndex)
	void* ConstructPtr  = nullptr;  // Type-erased pointer to the live Construct
	uint32_t TypeHash   = 0;        // FNV-1a of type name, for RPC dispatch
	int64_t PrefabIDRaw = 0;        // AssetID raw value of the spawning prefab
	uint8_t Generation  = 0;        // Bumped on destroy — matches GlobalConstructHandle::Generation
	uint8_t OwnerID     = 0;        // Fast access; redundant with NetworkID.NetOwnerID
	uint8_t _Pad[2]     = {};

	// Type-erased accessor for replication — set at AllocateNetRef time.
	// Fills out[] with the EntityHandle for each ConstructView.
	// nullptr if this Construct has no registered views.
	bool IsValid() const { return ConstructPtr != nullptr; }
	uint8_t GetGeneration() const { return Generation; }
	uint32_t GetOwnerID() const { return NetworkID.GetOwnerID(); }
};

