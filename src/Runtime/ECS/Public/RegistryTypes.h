#pragma once
#include <cstdint>

#include "SimFloat.h"

// Disable MSVC warning for anonymous structs in unions (C++11 standard feature)
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201) // nonstandard extension used: nameless struct/union
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif


static constexpr size_t NetOwnerID_Bits = 8;
static constexpr size_t MaxOwnerIDs     = 1u << NetOwnerID_Bits; // 256 possible owner IDs (0=server, 1-255=clients)
static constexpr size_t Generation_Bits = 16;
// EntityRef and SpawnFlags helpers will catch mismatches.
static constexpr size_t TypeKey_Bits     = 16;
static constexpr size_t UniqueIndex_Bits = 24;

// Lives in chunk storage, easily iterable, global index is derived, not stored.
union EntitySlotMeta
{
	uint32_t Value;

	struct
	{
		uint32_t Generation : Generation_Bits; // generation still lives with the entity, usable by logic and renderer
		uint32_t NetOwnerID : NetOwnerID_Bits; // MaxOwnerIDs possible owner IDs held per entity for comparison
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

	bool IsPredictedCreation() const { return NetFlags & 0x1; }
	// Signifies that following this uint32 is an EntityNetHandle this manifest is intended to modify
	bool HasEntityHandle() const { return NetFlags & 0x2; }
	bool PredictionResult() const { return NetFlags & 0x4; } // confirmed=1, rejected=0
};

static_assert(sizeof(EntityNetHandle) == 4, "NetID must be 4 bytes");

// ConstructNetHandle — 32-bit wire handle for Constructs. Mirrors EntityNetHandle.
// Internal engine code uses this exclusively. Gameplay holds ConstructRef (below).
union ConstructNetHandle
{
	uint32_t Value;

	struct
	{
		uint32_t NetOwnerID : NetOwnerID_Bits;
		uint32_t NetIndex   : UniqueIndex_Bits;
	};

	uint32_t GetOwnerID() const { return NetOwnerID; }
	uint32_t GetHandleIndex() const { return NetIndex; }
	bool IsServer() const { return NetOwnerID == 0; }
	bool IsLocal(uint8_t localClientID) const { return NetOwnerID == localClientID; }
	bool IsValid() const { return NetIndex != 0; }
};

static_assert(sizeof(ConstructNetHandle) == 4, "ConstructNetHandle must be 4 bytes");

// ---------------------------------------------------------------------------
// Two-tier handle design:
//
//   Internal (32-bit): EntityNetHandle / ConstructNetHandle
//     Used exclusively inside engine systems (ReplicationSystem, NetThread,
//     Registry internals). No generation — fast, compact, wire-safe for bulk
//     paths like StateCorrection where the server is the authority.
//
//   External (64-bit): EntityRef / ConstructRef
//     Used in gameplay code, RPCs, and client→server messages. Embeds
//     generation for ABA protection — stale handles are detected on the
//     server before any state mutation. The server validates:
//       NetToRecord[handle.NetIndex].Generation == ref.Generation
//
//   StateCorrectionEntry stays 32-bit (EntityNetHandle only). Server is the
//   authority on bulk corrections — no client-side stale-handle risk.
// ---------------------------------------------------------------------------
struct EntityRef
{
	EntityNetHandle Handle;
	uint16_t Generation; // matches GlobalEntityHandle::Generation at creation time
	uint16_t Flags;      // IsPredicted:1, IsOwned:1, reserved:14

	bool IsValid() const { return Handle.NetIndex != 0; }
	bool IsServer() const { return Handle.IsServer(); }
	bool IsLocal(uint8_t localID) const { return Handle.IsLocal(localID); }
	uint16_t GetGeneration() const { return Generation; }
};

static_assert(sizeof(EntityRef) == 8, "EntityRef must be 8 bytes");
static_assert(Generation_Bits <= 16, "EntityRef::Generation is uint16_t — increase it if Generation_Bits > 16");

struct ConstructRef
{
	ConstructNetHandle Handle;
	uint16_t Generation; // matches GlobalConstructHandle::Generation at creation time
	uint16_t Flags;      // IsPredicted:1, IsOwned:1, reserved:14

	bool IsValid() const { return Handle.NetIndex != 0; }
	bool IsServer() const { return Handle.IsServer(); }
	bool IsLocal(uint8_t localID) const { return Handle.IsLocal(localID); }
	uint16_t GetGeneration() const { return Generation; }
};

static_assert(sizeof(ConstructRef) == 8, "ConstructRef must be 8 bytes");

using EntityCacheHandle = uint32_t;

// Server-authoritative transform snapshot — queued during rollback for post-resim comparison.
// If the entity's resimmed position still diverges from PosXYZ, the correction is applied.
struct EntityTransformCorrection
{
	uint32_t NetHandle;
	uint32_t ClientFrame;
	SimFloat PosX, PosY, PosZ;
	SimFloat RotQx, RotQy, RotQz, RotQw;
};
