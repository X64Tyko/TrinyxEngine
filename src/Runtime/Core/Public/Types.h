#pragma once
#include <array>
#include <bitset>
#include <cstdint>
#include <functional>
#include <cmath>

// Cross-platform force inline macro
#ifdef _MSC_VER
#define FORCE_INLINE __forceinline
#else
#define FORCE_INLINE inline __attribute__((always_inline))
#endif

// Disable MSVC warning for anonymous structs in unions (C++11 standard feature)
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201) // nonstandard extension used: nameless struct/union
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

// allows changing the behavior of the FieldProxy
enum class FieldWidth : uint8_t
{
	Scalar,
	Wide,
	WideMask
};

// Identifies which SoA ring buffer tier an entity's fields live in.
// uint8_t backing for forward-compatibility (Static, Cold tiers planned).
enum class CacheTier : uint8_t
{
	None      = 0,
	Volatile  = 1, // 5-frame ring buffer — cosmetic entities, no rollback
	Temporal  = 2, // N-frame ring buffer — simulation-authoritative, rollback-capable
	Universal = 3, // N-frame ring buffer - Grows as needed and contains components all entities share

	MAX
};

template <template <FieldWidth> class Derived, FieldWidth WIDTH = FieldWidth::Scalar>
using MaskTemplate = Derived<WIDTH>;

// Math types
struct Vector3
{
	float x, y, z;

	Vector3()
		: x(0)
		, y(0)
		, z(0)
	{
	}

	Vector3(float x, float y, float z)
		: x(x)
		, y(y)
		, z(z)
	{
	}

	Vector3 operator+(const Vector3& other) const { return Vector3(x + other.x, y + other.y, z + other.z); }
	Vector3 operator-(const Vector3& other) const { return Vector3(x - other.x, y - other.y, z - other.z); }
	Vector3 operator*(float scalar) const { return Vector3(x * scalar, y * scalar, z * scalar); }

	float Length() const { return std::sqrt(x * x + y * y + z * z); }

	Vector3 Normalized() const
	{
		float len = Length();
		return len > 0 ? Vector3(x / len, y / len, z / len) : Vector3();
	}
};

struct Matrix4
{
	float m[16]; // Column-major order

	Matrix4()
	{
		for (int i = 0; i < 16; i++) m[i] = 0;
		m[0] = m[5] = m[10] = m[15] = 1.0f; // Identity
	}

	static Matrix4 Identity() { return Matrix4(); }

	// Column-major multiply: result = a * b
	// Both forms compile to the same code; the named static is here so you can
	// grep for it when profiling and swap in an intrinsic version if needed.
	static Matrix4 Multiply(const Matrix4& a, const Matrix4& b)
	{
		Matrix4 r;
		for (int col = 0; col < 4; ++col)
			for (int row = 0; row < 4; ++row)
			{
				float sum = 0.0f;
				for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[col * 4 + k];
				r.m[col * 4 + row] = sum;
			}
		return r;
	}

	Matrix4 operator*(const Matrix4& o) const { return Multiply(*this, o); }
};

// Component type ID - numeric identifier for each component type (0-255)
using ComponentTypeID = uint32_t;

// Component Signature definition as a bitset - tracks which components are present
static constexpr size_t MAX_COMPONENTS                    = 256;
static constexpr size_t MAX_TEMPORAL_FIELDS_PER_COMPONENT = 64;                                                 // Max decomposed temporal fields per component
static constexpr size_t MAX_FIELD_ARRAYS                  = MAX_COMPONENTS * MAX_TEMPORAL_FIELDS_PER_COMPONENT; // Max total field arrays across all components
// Upper bound on field arrays for any single archetype (temporal + non-temporal).
// Chunk::MAX_TEMPORAL_FIELDS caps temporal fields at 64; this adds headroom for non-temporal.
static constexpr size_t MAX_FIELDS_PER_ARCHETYPE = 64;
using ComponentSignature                         = std::bitset<MAX_COMPONENTS>;
using ClassID                                    = uint16_t;
static constexpr size_t MAX_CLASS_COUNT          = 4096; // based on size of TypeID in Entity header hardcoded for testing

// roughly 32 entities per chunk if they have 64 fields of 4bytes each all in chunk storage.
constexpr uint32_t CHUNK_SIZE = 256 * MAX_FIELDS_PER_ARCHETYPE * 4;

// Global counters (defined in TrinyxEngine.cpp)
namespace Internal
{
	extern uint32_t g_GlobalComponentCounter;

	extern std::array<uint8_t, static_cast<size_t>(CacheTier::MAX)> g_TemporalComponentCounter;
}

static constexpr size_t NetOwnerID_Bits  = 8;
static constexpr size_t Generation_Bits  = 16;
static constexpr size_t TypeKey_Bits     = 16;
static constexpr size_t UniqueIndex_Bits = 24;

// Lives in chunk storage, easily iterable, global index is derived, not stored.
union EntityIDNew
{
	uint32_t Value;

	struct
	{
		uint32_t Generation : Generation_Bits; // generation still lives with the entity, usable by logic and renderer
		uint32_t NetOwnerID : NetOwnerID_Bits; // still 256 possible owner IDs held per entity for comparison
		uint32_t reserved   : 8;               // genuinely not sure what else they'd need yet
	};
};

static_assert(sizeof(EntityIDNew) == 4, "EntityIDNew must be 4 bytes");

union NetID
{
	uint32_t Value;

	struct
	{
		uint32_t NetOwnerID : NetOwnerID_Bits;  // With this in the NetID we have an entity index and an owner, this means that on the server we have a single array of net entities, each owners local entities live contiguously. 0 is global
		uint32_t NID        : UniqueIndex_Bits; // Net ID size matches the EntityHandle Index. This could probably be smaller to allow packing some other data specifically for networking as 16M networked entities is a lot.
	};
};

using HandleID = NetID;

static_assert(sizeof(NetID) == 4, "NetID must be 4 bytes");
/*
struct EntityRecord
{
	NetID NetworkID; // deterministic network replicated entity ID
	HandleID Handle; // Index into the entity Handle array 
	
	uint32_t CacheEntityIndex; // Cache slab index
};
*/
struct PredictionKey
{
	NetID RequestedKey;
	NetID ReceivedKey;

	// stubs, general ideas for allowing the key to handle prediction related logic via lambda capture.
	void OnPredictionRejected();
	void OnPredictionAccepted();
};

/* Prediction model
 * Client creates a PredictionKey. this key has a requested key sent to the server. When the prediction key is sent back we use ReceivedKey to determine if it was accepted or rejected and set the proper net key as well as fire handlers for the outcome. 
 * Server can reject or accept the request. If accepted it will return the validation as well as the netID replacement if the predicted ID was wrong.
 * Client recieves the confirmation and its predicted netID to resolve the prediction, it can replace its netID after if the server provided a different one.
 * */

// We rename the current idea of GlobalEntityIndex to CacheEntityIndex and make global truly global.
using GlobalEntityIndex = uint32_t;

// just using these to note that these lookups will live in some form later.
using NetEntityRecord   = std::unordered_map<NetID, GlobalEntityIndex>;    // If lookup is necessary from a net packet, netID -> Entity Record gives us immediate access to indexes with 1 ptr
using LocalEntityRecord = std::unordered_map<uint32_t, GlobalEntityIndex>; // Convert local ID idx -> netID when building a net packet
using CacheEntityRecord = std::unordered_map<uint32_t, GlobalEntityIndex>;

// GlobalEntityRecord = std::vector<EntityRecord>(MAX_CACHED_ENTITIES); // we can have more than max cached entities total, so we allow this to grow, but it is our global entity lookup, all entities exist here.

// held by objects that need to know about a specific entity and for networking
namespace Trinyx_Internal
{
	union EntityHandle
	{
		uint64_t Value;

		struct
		{
			uint64_t Index      : UniqueIndex_Bits; // The index into the associated map for this entity type. This EntityHandle is not intended to be used
			uint64_t Generation : Generation_Bits;  // used to compare against the slot generation, mismatch means our handle is old
			uint64_t TypeKey    : TypeKey_Bits;     // This allows us to construct server spawned entities client side straight from the net handle
			uint64_t NetOwnerID : NetOwnerID_Bits;  // This allows us to filter by ownerID when networking without a single lookup
		};
	};
}

static_assert(sizeof(Trinyx_Internal::EntityHandle) == 8, "EntityHandle must be 8 bytes");

using LocalEntityHandle = Trinyx_Internal::EntityHandle;
using NetEntityHandle   = Trinyx_Internal::EntityHandle;

// EntityID - 64-bit smart handle with embedded metadata
// Swappable design: Implement GetIndex(), IsValid(), operator== for custom implementations
union EntityID
{
	uint64_t Value;

	// Bitfield layout
	struct
	{
		uint64_t Index      : 20; // 1 Million entities (array slot)
		uint64_t Generation : 16; // 65k recycles (server-grade stability)
		uint64_t TypeID     : 12; // 4k class types (function dispatch)
		uint64_t OwnerID    : 8;  // 256 owners (network routing)
		uint64_t IsStatic   : 1;  // Static Entity Flag
		uint64_t MetaFlags  : 7;  // Reserved for future use
	};

	// Required interface (for swappability)
	uint32_t GetIndex() const { return static_cast<uint32_t>(Index); }
	uint16_t GetGeneration() const { return static_cast<uint16_t>(Generation); }
	uint16_t GetTypeID() const { return static_cast<uint16_t>(TypeID); }
	uint8_t GetOwnerID() const { return static_cast<uint8_t>(OwnerID); }
	bool GetIsStatic() const { return IsStatic; }

	bool IsValid() const { return Value != 0; }

	static EntityID Invalid()
	{
		EntityID Id;
		Id.Value = 0;
		return Id;
	}

	// Comparison operators
	bool operator==(const EntityID& Other) const { return Value == Other.Value; }
	bool operator!=(const EntityID& Other) const { return Value != Other.Value; }

	// Network/ownership helpers
	bool IsServer() const { return OwnerID == 0; }
	bool IsLocal(uint8_t LocalClientID) const { return OwnerID == LocalClientID; }
};


#ifdef _MSC_VER
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// Hash specialization for std::unordered_map
namespace std
{
	template <>
	struct hash<EntityID>
	{
		size_t operator()(const EntityID& Id) const noexcept
		{
			return hash<uint64_t>()(Id.Value);
		}
	};
}
