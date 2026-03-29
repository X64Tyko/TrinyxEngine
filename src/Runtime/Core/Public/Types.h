#pragma once
#include <array>
#include "FixedBitset.h"
#include <cstdint>
#include <functional>
#include <cmath>

// Cross-platform force inline macro
#ifdef _MSC_VER
#define FORCE_INLINE __forceinline
#else
#define FORCE_INLINE inline __attribute__((always_inline))
#endif

// Cross-platform bit scan intrinsics
#ifdef _MSC_VER
#include <intrin.h>
// Count trailing zeros (32-bit)
#define TNX_CTZ32(x) ([](uint32_t val) -> uint32_t { \
	unsigned long idx; \
	_BitScanForward(&idx, val); \
	return static_cast<uint32_t>(idx); \
}(x))
// Count trailing zeros (64-bit)
#define TNX_CTZ64(x) ([](uint64_t val) -> uint32_t { \
	unsigned long idx; \
	_BitScanForward64(&idx, val); \
	return static_cast<uint32_t>(idx); \
}(x))
#else
// GCC/Clang builtins
#define TNX_CTZ32(x) __builtin_ctz(x)
#define TNX_CTZ64(x) __builtin_ctzll(x)
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
using ComponentSignature                         = FixedBitset<MAX_COMPONENTS>;
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

// GlobalEntityRecord = std::vector<EntityRecord>(MAX_CACHED_ENTITIES); // we can have more than max cached entities total, so we allow this to grow, but it is our global entity lookup, all entities exist here.

/* Old Entity ID
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
*/

#ifdef _MSC_VER
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
