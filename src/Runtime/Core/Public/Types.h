#pragma once
#include <array>
#include "FixedBitset.h"
#include <cstdint>
#include <functional>
#include <type_traits>

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

// Engine mode — determines what subsystems are initialized.
// Defined here (not in NetTypes.h) so it is available regardless of TNX_ENABLE_NETWORK.
enum class EngineMode : uint8_t
{
	Standalone, // No networking — default for singleplayer / editor
	Client,     // Connects to remote server, renders locally
	Server,     // Headless — no window/Vulkan/renderer
	Host,       // Server + local client in one process (PIE default)
};

[[maybe_unused]] static const char* EngineModeNames[] = {
	"Standalone",
	"Client",
	"Server",
	"Host",
};

// Identifies the lifetime tier of a Construct — determines what survives
// level transitions, World resets, and session teardown.
// FlowManager uses this to enforce destruction/survival on transitions.
enum class ConstructLifetime : uint8_t
{
	Level,      // Destroyed when the Level unloads
	World,      // Destroyed when the World resets
	Session,    // Survives World reset. Destroyed when the session ends.
	Persistent, // Survives everything. Destroyed only explicitly.
};

#include "SimFloat.h"

namespace TVecDetail
{
	template <typename Dst, typename Src>
	FORCE_INLINE Dst ConvertScalar(const Src& s)
	{
		if constexpr (std::is_same_v<Dst, Src>) return s;
		else if constexpr (std::is_same_v<Dst, float>) return s.ToFloat();
		else if constexpr (std::is_same_v<Src, float>) return Dst(Fixed32::FromFloat(s));
		else return Dst(Fixed32::FromFloat(s.ToFloat()));
	}
}

// Forward declaration for Sqrt – defined in SimFloat.h
template <typename T>
SimFloatImpl<T> Sqrt(SimFloatImpl<T> x);

template <template <FieldWidth> class Derived, FieldWidth WIDTH = FieldWidth::Scalar>
using MaskTemplate = Derived<WIDTH>;

// Math types
template <typename VecType = SimFloat>
struct TVector3
{
	VecType x, y, z;

	TVector3()
		: x(0)
		, y(0)
		, z(0)
	{
	}

	// Converting constructor – accepts any three arguments that can be
	// converted to floatType.  Avoids duplication when SimFloat == float.
	template <typename... Args,
			  std::enable_if_t<sizeof...(Args) == 3 &&
							   (std::is_convertible_v<Args, VecType> && ...), int> = 0>
	TVector3(Args... args)
	{
		VecType arr[] = {static_cast<VecType>(args)...};
		x             = arr[0];
		y             = arr[1];
		z             = arr[2];
	}

	// Cross‑type copy: convert each component from any other TVector3.
#ifdef TNX_FIXED_IMPLICIT_FLOAT
	template <typename Other>
	TVector3(const TVector3<Other>& other)
		: TVector3(other.template CastTo<VecType>())
	{
	}
#endif

	// Compiler-generated copy and move assignment are fine for same-type.
	// This template handles cross‑type assignment (e.g., TVector3<Fixed32> <- TVector3<float>).
#ifdef TNX_FIXED_IMPLICIT_FLOAT
	template <typename Other>
	TVector3& operator=(const TVector3<Other>& other)
	{
		*this = other.template CastTo<VecType>();
		return *this;
	}
#endif

	// ── Explicit conversions ──
	template <typename Dst>
	TVector3<Dst> CastTo() const
	{
		return TVector3<Dst>(
			TVecDetail::ConvertScalar<Dst>(x),
			TVecDetail::ConvertScalar<Dst>(y),
			TVecDetail::ConvertScalar<Dst>(z));
	}

	TVector3<float> ToFloat() const { return CastTo<float>(); }
	TVector3<SimFloat> ToSim() const { return CastTo<SimFloat>(); }

	TVector3& operator+=(const TVector3& other)
	{
		x += other.x;
		y += other.y;
		z += other.z;
		return *this;
	}

	TVector3& operator-=(const TVector3& other)
	{
		x -= other.x;
		y -= other.y;
		z -= other.z;
		return *this;
	}

	TVector3& operator*=(VecType scalar)
	{
		x *= scalar;
		y *= scalar;
		z *= scalar;
		return *this;
	}

	TVector3& operator/=(VecType scalar)
	{
		x /= scalar;
		y /= scalar;
		z /= scalar;
		return *this;
	}

	TVector3 operator+(const TVector3<VecType>& other) const { return TVector3(x + other.x, y + other.y, z + other.z); }
	TVector3 operator-(const TVector3<VecType>& other) const { return TVector3(x - other.x, y - other.y, z - other.z); }
	// Right‑multiply by any type convertible to floatType
	template <typename T>
	TVector3 operator*(const T& scalar) const
	{
		VecType s = static_cast<VecType>(scalar);
		return TVector3(x * s, y * s, z * s);
	}

	template <typename T>
	TVector3 operator/(const T& scalar) const
	{
		VecType s = static_cast<VecType>(scalar);
		return TVector3(x / s, y / s, z / s);
	}

	TVector3 operator-() const { return TVector3(-x, -y, -z); }
	// Left‑multiply by any type convertible to floatType
	template <typename T>
	friend TVector3 operator*(const T& scalar, const TVector3& v)
	{
		VecType s = static_cast<VecType>(scalar);
		return TVector3(s * v.x, s * v.y, s * v.z);
	}

	// ── Squared magnitude & distance (no sqrt) ──
	VecType LengthSqr() const { return x * x + y * y + z * z; }
	VecType DistanceSqr(const TVector3& o) const { return (*this - o).LengthSqr(); }
	VecType Distance(const TVector3& o) const { return Sqrt(DistanceSqr(o)); }
	bool IsNearZero(VecType eps2 = VecType(1e-8f)) const { return LengthSqr() <= eps2; }

	VecType Length() const
	{
		return Sqrt(x * x + y * y + z * z);
	}

	float LengthF() const
	{
		if constexpr (std::is_same_v<VecType, SimFloat>) return static_cast<float>(Length());
		else return static_cast<float>(Length());
	}

	TVector3 Normalized() const
	{
		VecType len = Length();
		return len > VecType(0) ? TVector3(x / len, y / len, z / len) : TVector3();
	}
};

using Vector3  = TVector3<>;
using Vector3f = TVector3<float>;

// Free functions
template <typename VecType>
VecType Dot(const TVector3<VecType>& a, const TVector3<VecType>& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

template <typename VecType>
TVector3<VecType> Cross(const TVector3<VecType>& a, const TVector3<VecType>& b)
{
	return TVector3<VecType>(
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x);
}

template <typename MatType = SimFloat>
struct TMatrix4
{
	MatType m[16]; // Column-major order

	TMatrix4()
	{
		for (int i = 0; i < 16; i++) m[i] = 0;
		m[0] = m[5] = m[10] = m[15] = 1; // Identity
	}

	static TMatrix4 Identity() { return TMatrix4(); }

	// Column-major multiply: result = a * b
	// Both forms compile to the same code; the named static is here so you can
	// grep for it when profiling and swap in an intrinsic version if needed.
	static TMatrix4 Multiply(const TMatrix4& a, const TMatrix4& b)
	{
		TMatrix4 r;
		for (int col = 0; col < 4; ++col)
			for (int row = 0; row < 4; ++row)
			{
				MatType sum(0);
				for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[col * 4 + k];
				r.m[col * 4 + row] = sum;
			}
		return r;
	}
	
	MatType& operator[](int i) { return m[i]; }

	TMatrix4 operator*(const TMatrix4& o) const { return Multiply(*this, o); }


	// ── Explicit conversions ──
	template <typename Dst>
	TMatrix4<Dst> CastTo() const
	{
		TMatrix4<Dst> r;
		for (int i = 0; i < 16; ++i) r.m[i] = TVecDetail::ConvertScalar<Dst>(m[i]);
		return r;
	}

	TMatrix4<float> ToFloat() const { return CastTo<float>(); }
	TMatrix4<SimFloat> ToSim() const { return CastTo<SimFloat>(); }
};

using Matrix4  = TMatrix4<>;
using Matrix4f = TMatrix4<float>;

// Component type ID - numeric identifier for each component type (0-255)
using ComponentTypeID = uint32_t;

// Per-cache slot index — which slot a component occupies within its associated ComponentCacheBase.
// Distinct from ComponentTypeID: MetaFlags may be slot 0 in the Temporal cache but a different
// component could be slot 0 in the Volatile cache. Same underlying width as StaticTemporalIndex().
using CacheSlotID = uint8_t;

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
	extern uint8_t g_GlobalMixinCounter; // user mixin IDs, starts at 128

	extern std::array<uint8_t, static_cast<size_t>(CacheTier::MAX)> g_TemporalComponentCounter;
}

#ifdef _MSC_VER
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
