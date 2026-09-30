#pragma once
#include <array>
#include "FixedBitset.h"
#include <cstdint>
#include <functional>
#include <type_traits>

/** @addtogroup core
 *  @{
 */

/// @brief Force-inline attribute — `__forceinline` on MSVC, `always_inline` on GCC/Clang.
#ifdef _MSC_VER
#define FORCE_INLINE __forceinline
#else
#define FORCE_INLINE inline __attribute__((always_inline))
#endif

// Cross-platform bit-scan intrinsics
#ifdef _MSC_VER
#include <intrin.h>
/// @brief Count trailing zeros (32-bit) — returns the index of the least significant set bit.
#define TNX_CTZ32(x) ([](uint32_t val) -> uint32_t { \
	unsigned long idx;                               \
	_BitScanForward(&idx, val);                      \
	return static_cast<uint32_t>(idx);               \
}(x))
/// @brief Count trailing zeros (64-bit) — returns the index of the least significant set bit.
#define TNX_CTZ64(x) ([](uint64_t val) -> uint32_t { \
	unsigned long idx;                               \
	_BitScanForward64(&idx, val);                    \
	return static_cast<uint32_t>(idx);               \
}(x))
#else
#define TNX_CTZ32(x) __builtin_ctz(x)
#define TNX_CTZ64(x) __builtin_ctzll(x)
#endif

// Cross-platform popcount intrinsics
#ifdef _MSC_VER
/// @brief Count set bits (32-bit).
#define TNX_POPCOUNT32(x) static_cast<uint32_t>(__popcnt(static_cast<unsigned int>(x)))
#else
#define TNX_POPCOUNT32(x) static_cast<uint32_t>(__builtin_popcount(static_cast<unsigned int>(x)))
#endif

// Suppress warnings for anonymous structs in unions used by math types.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4201) // nonstandard extension used: nameless struct/union
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

/// @brief Selects the storage and update path for a FieldProxy.
enum class FieldWidth : uint8_t
{
	Scalar,   ///< Single-entity scalar update path.
	Wide,     ///< AVX2 8-wide SIMD unconditional store.
	WideMask, ///< AVX2 8-wide SIMD masked store (gated on the Active flag).
};

/// @brief Identifies which SoA ring-buffer tier an entity's fields live in.
/// @note When @c TNX_ENABLE_ROLLBACK is disabled, Temporal is treated as Volatile at no memory cost.
enum class CacheTier : uint8_t
{
	None      = 0, ///< No SoA tier — cold/chunk-only data, not iterable by wide sweeps.
	Volatile  = 1, ///< 3-frame ring buffer — cosmetic entities, ambient AI, particles (no rollback).
	Temporal  = 2, ///< N-frame ring buffer — simulation-authoritative, rollback-capable.
	Universal = 3, ///< N-frame ring buffer — grows as needed; fields shared by all entities.

	MAX
};

/// @brief Determines what subsystems are initialized for this process.
enum class EngineMode : uint8_t
{
	Standalone, ///< No networking — singleplayer / editor default.
	Client,     ///< Connects to a remote Authority; renders locally.
	Server,     ///< Headless Authority — no window, no Vulkan, no renderer.
	Host,       ///< Authority + Owner in one process — listen-server / PIE default.
};

[[maybe_unused]] static const char* EngineModeNames[] = {
	"Standalone",
	"Client",
	"Server",
	"Host",
};

/// @brief Lifetime tier of a Construct — determines survival across level transitions and World resets.
/// @note FlowManager enforces construction and destruction order during transitions.
enum class ConstructLifetime : uint8_t
{
	Level,      ///< Destroyed when the Level unloads.
	World,      ///< Destroyed when the World resets.
	Session,    ///< Survives World reset; destroyed when the session ends.
	Persistent, ///< Survives everything; destroyed only explicitly.
};

#include "SimFloat.h"

namespace TVecDetail
{
template <typename Dst, typename Src>
FORCE_INLINE Dst ConvertScalar(const Src& s)
{
	if constexpr (std::is_same_v<Dst, Src>)
		return s;
	else if constexpr (std::is_same_v<Dst, float>)
		return s.ToFloat();
	else if constexpr (std::is_same_v<Src, float>)
		return Dst(Fixed32::FromFloat(s));
	else
		return Dst(Fixed32::FromFloat(s.ToFloat()));
}
} // namespace TVecDetail

// Forward declaration for Sqrt – defined in SimFloat.h
template <typename T>
SimFloatImpl<T> Sqrt(SimFloatImpl<T> x);

template <template <FieldWidth> class Derived, FieldWidth WIDTH = FieldWidth::Scalar>
using MaskTemplate = Derived<WIDTH>;

/**
 * @brief 3-component vector parameterized on a scalar type.
 *
 * @tparam VecType Scalar element type — defaults to @c SimFloat (Fixed32 or float depending on build).
 *
 * Cross-type construction and assignment are available when @c TNX_FIXED_IMPLICIT_FLOAT is defined.
 * Use @c CastTo<Dst>() or the @c ToFloat() / @c ToSim() helpers for explicit conversions.
 */
template <typename VecType = SimFloat>
struct TVector3
{
	VecType x, y, z;

	TVector3()
		: x(0),
		  y(0),
		  z(0)
	{
	}

	// Variadic ctor — accepts any three arguments convertible to VecType.
	// Avoids duplicate ctors when SimFloat == float.
	template <typename... Args,
		std::enable_if_t<sizeof...(Args) == 3 && (std::is_convertible_v<Args, VecType> && ...), int> = 0>
	TVector3(Args... args)
	{
		VecType arr[] = { static_cast<VecType>(args)... };
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

	/// @brief Explicit per-component type conversion.
	template <typename Dst>
	TVector3<Dst> CastTo() const
	{
		return TVector3<Dst>(
			TVecDetail::ConvertScalar<Dst>(x),
			TVecDetail::ConvertScalar<Dst>(y),
			TVecDetail::ConvertScalar<Dst>(z));
	}

	TVector3<float> ToFloat() const { return CastTo<float>(); }     ///< @brief Convert to float components.
	TVector3<SimFloat> ToSim() const { return CastTo<SimFloat>(); } ///< @brief Convert to SimFloat components.

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
	// Right‑multiply by any type convertible to VecType
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
	// Left‑multiply by any type convertible to VecType
	template <typename T>
	friend TVector3 operator*(const T& scalar, const TVector3& v)
	{
		VecType s = static_cast<VecType>(scalar);
		return TVector3(s * v.x, s * v.y, s * v.z);
	}

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
		if constexpr (std::is_same_v<VecType, SimFloat>)
			return static_cast<float>(Length());
		else
			return static_cast<float>(Length());
	}

	TVector3 Normalized() const
	{
		VecType len = Length();
		return len > VecType(0) ? TVector3(x / len, y / len, z / len) : TVector3();
	}
};

using Vector3  = TVector3<>;      ///< Simulation-space 3-vector (SimFloat components).
using Vector3f = TVector3<float>; ///< Float-component 3-vector for render/upload code.

/// @brief Dot product of two same-type vectors.
template <typename VecType>
VecType Dot(const TVector3<VecType>& a, const TVector3<VecType>& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

/// @brief Cross product of two same-type vectors.
template <typename VecType>
TVector3<VecType> Cross(const TVector3<VecType>& a, const TVector3<VecType>& b)
{
	return TVector3<VecType>(
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x);
}

/**
 * @brief Unit quaternion parameterized on a scalar type.
 *
 * @tparam VecType Scalar element type — defaults to @c SimFloat.
 *
 * Stored as (x, y, z, w) with the scalar part in @c w. Hamilton product convention.
 */
template <typename VecType = SimFloat>
struct TQuat
{
	VecType x, y, z, w;

	TQuat()
		: x(0),
		  y(0),
		  z(0),
		  w(1)
	{
	}
	TQuat(VecType x_, VecType y_, VecType z_, VecType w_)
		: x(x_),
		  y(y_),
		  z(z_),
		  w(w_)
	{
	}

	static TQuat Identity() { return TQuat(); }

	TQuat Conjugate() const { return TQuat(-x, -y, -z, w); }
	VecType LengthSqr() const { return x * x + y * y + z * z + w * w; }

	TQuat Normalized() const
	{
		VecType len = Sqrt(LengthSqr());
		return len > VecType(0) ? TQuat(x / len, y / len, z / len, w / len) : Identity();
	}

	/// @brief Hamilton product: @c this * o.
	TQuat operator*(const TQuat& o) const
	{
		return TQuat(
			w * o.x + x * o.w + y * o.z - z * o.y,
			w * o.y - x * o.z + y * o.w + z * o.x,
			w * o.z + x * o.y - y * o.x + z * o.w,
			w * o.w - x * o.x - y * o.y - z * o.z);
	}

	/// @brief Rotate vector @p v by this unit quaternion: v + 2w(q×v) + 2(q×(q×v)).
	TVector3<VecType> Rotate(const TVector3<VecType>& v) const
	{
		TVector3<VecType> qv{ x, y, z };
		TVector3<VecType> t = Cross(qv, v) * VecType(2);
		return v + t * w + Cross(qv, t);
	}

	/// @brief Explicit per-component type conversion.
	template <typename Dst>
	TQuat<Dst> CastTo() const
	{
		return TQuat<Dst>(
			TVecDetail::ConvertScalar<Dst>(x),
			TVecDetail::ConvertScalar<Dst>(y),
			TVecDetail::ConvertScalar<Dst>(z),
			TVecDetail::ConvertScalar<Dst>(w));
	}

	TQuat<float> ToFloat() const { return CastTo<float>(); }     ///< @brief Convert to float components.
	TQuat<SimFloat> ToSim() const { return CastTo<SimFloat>(); } ///< @brief Convert to SimFloat components.
};

using Quat  = TQuat<>;      ///< Simulation-space quaternion (SimFloat components).
using Quatf = TQuat<float>; ///< Float-component quaternion for render/upload code.

/**
 * @brief 4×4 column-major matrix parameterized on a scalar type.
 *
 * @tparam MatType Scalar element type — defaults to @c SimFloat.
 *
 * Storage: @c m[col * 4 + row]. Default-constructed as identity.
 */
template <typename MatType = SimFloat>
struct TMatrix4
{
	MatType m[16]; ///< Column-major storage: m[col * 4 + row].

	TMatrix4()
	{
		for (int i = 0; i < 16; i++)
			m[i] = 0;
		m[0] = m[5] = m[10] = m[15] = 1; // Identity
	}

	static TMatrix4 Identity() { return TMatrix4(); }

	/// @brief Column-major multiply: result = a * b.
	static TMatrix4 Multiply(const TMatrix4& a, const TMatrix4& b)
	{
		TMatrix4 r;
		for (int col = 0; col < 4; ++col)
			for (int row = 0; row < 4; ++row)
			{
				MatType sum(0);
				for (int k = 0; k < 4; ++k)
					sum += a.m[k * 4 + row] * b.m[col * 4 + k];
				r.m[col * 4 + row] = sum;
			}
		return r;
	}

	MatType& operator[](int i) { return m[i]; }

	TMatrix4 operator*(const TMatrix4& o) const { return Multiply(*this, o); }

	/// @brief Explicit per-element type conversion.
	template <typename Dst>
	TMatrix4<Dst> CastTo() const
	{
		TMatrix4<Dst> r;
		for (int i = 0; i < 16; ++i)
			r.m[i] = TVecDetail::ConvertScalar<Dst>(m[i]);
		return r;
	}

	TMatrix4<float> ToFloat() const { return CastTo<float>(); }     ///< @brief Convert to float elements.
	TMatrix4<SimFloat> ToSim() const { return CastTo<SimFloat>(); } ///< @brief Convert to SimFloat elements.
};

using Matrix4  = TMatrix4<>;      ///< Simulation-space 4×4 matrix (SimFloat elements).
using Matrix4f = TMatrix4<float>; ///< Float-element 4×4 matrix for render/upload code.

using ComponentTypeID = uint32_t; ///< Numeric identifier for a component type.

/// @brief Slot index within a ComponentCacheBase — which SoA column a component occupies.
/// @note Distinct from ComponentTypeID: slot 0 may refer to different components in different caches.
using CacheSlotID = uint8_t;

static constexpr size_t MAX_COMPONENTS                    = 256;                                                ///< Maximum distinct component types.
static constexpr size_t MAX_TEMPORAL_FIELDS_PER_COMPONENT = 64;                                                 ///< Max decomposed temporal SoA fields per component.
static constexpr size_t MAX_FIELD_ARRAYS                  = MAX_COMPONENTS * MAX_TEMPORAL_FIELDS_PER_COMPONENT; ///< Max total field arrays across all components.
/// @brief Upper bound on field arrays for any single archetype (temporal + non-temporal).
static constexpr size_t MAX_FIELDS_PER_ARCHETYPE = 64;

using ComponentSignature                = FixedBitset<MAX_COMPONENTS>; ///< Bitmask tracking which component types are present on an archetype.
using ClassID                           = uint16_t;                    ///< Per-Construct-type numeric class identifier.
static constexpr size_t MAX_CLASS_COUNT = 4096;                        ///< Hard limit based on the ClassID field width in Entity headers.

/// @brief Nominal chunk size in bytes — ~32 entities if each has 64 fields of 4 bytes in chunk storage.
constexpr uint32_t CHUNK_SIZE = 256 * MAX_FIELDS_PER_ARCHETYPE * 4;

/// @brief Engine-internal global counters. Do not access directly from gameplay code.
namespace Internal
{
extern uint32_t g_GlobalComponentCounter; ///< Monotonically increasing component type counter.
extern uint8_t g_GlobalMixinCounter;      ///< User mixin IDs; starts at 128 to avoid collisions with engine mixins.

/// @brief Per-CacheTier component slot counters, indexed by CacheTier value.
extern std::array<uint8_t, static_cast<size_t>(CacheTier::MAX)> g_TemporalComponentCounter;
} // namespace Internal

#ifdef _MSC_VER
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

/** @} */
