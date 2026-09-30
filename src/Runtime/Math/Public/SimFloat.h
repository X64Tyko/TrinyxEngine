#pragma once
#include <cmath>
#include <immintrin.h>
#include <type_traits>

#include "Fixed32.h"
#include "FixedTrig.h"

// Cross-platform force inline macro
#ifndef FORCE_INLINE
#ifdef _MSC_VER
#define FORCE_INLINE __forceinline
#else
#define FORCE_INLINE inline __attribute__((always_inline))
#endif
#endif

// --- SimFloat Implementation ---
template <typename T>
struct SimFloatImpl;

// Specialization for float
template <>
struct SimFloatImpl<float>
{
	float value;

	// Construction
	SimFloatImpl()
		: value(0.0f)
	{
	}

	constexpr SimFloatImpl(float f)
		: value(f)
	{
	}

	SimFloatImpl(double d)
		: value(static_cast<float>(d))
	{
	}

	SimFloatImpl(int32_t i)
		: value(static_cast<float>(i))
	{
	}

	explicit SimFloatImpl(const Fixed32& fx)
		: value(fx.ToFloat())
	{
	}

	// Accessors
	float ToFloat() const { return value; }
	Fixed32 ToFixed() const { return Fixed32::FromFloat(value); }
	double ToDouble() const { return static_cast<double>(value); }

	// Unary operators
	SimFloatImpl operator+() const { return *this; }
	SimFloatImpl operator-() const { return SimFloatImpl(-value); }

	SimFloatImpl& operator++()
	{
		++value;
		return *this;
	}

	SimFloatImpl& operator--()
	{
		--value;
		return *this;
	}

	SimFloatImpl operator++(int) { return SimFloatImpl(value++); }
	SimFloatImpl operator--(int) { return SimFloatImpl(value--); }

	// Compound assignment
	SimFloatImpl& operator+=(const SimFloatImpl& rhs)
	{
		value += rhs.value;
		return *this;
	}

	SimFloatImpl& operator-=(const SimFloatImpl& rhs)
	{
		value -= rhs.value;
		return *this;
	}

	SimFloatImpl& operator*=(const SimFloatImpl& rhs)
	{
		value *= rhs.value;
		return *this;
	}

	SimFloatImpl& operator/=(const SimFloatImpl& rhs)
	{
		value /= rhs.value;
		return *this;
	}

	SimFloatImpl& operator+=(int32_t rhs)
	{
		value += rhs;
		return *this;
	}

	SimFloatImpl& operator-=(int32_t rhs)
	{
		value -= rhs;
		return *this;
	}

	SimFloatImpl& operator*=(int32_t rhs)
	{
		value *= rhs;
		return *this;
	}

	SimFloatImpl& operator/=(int32_t rhs)
	{
		value /= rhs;
		return *this;
	}

	// Assignment operators
	SimFloatImpl& operator=(float f)
	{
		value = f;
		return *this;
	}

	SimFloatImpl& operator=(double d)
	{
		value = static_cast<float>(d);
		return *this;
	}

	SimFloatImpl& operator=(int32_t i)
	{
		value = static_cast<float>(i);
		return *this;
	}

	SimFloatImpl& operator=(const Fixed32& fx)
	{
		value = fx.ToFloat();
		return *this;
	}

	// Comparison
	friend auto operator<=>(const SimFloatImpl&, const SimFloatImpl&) = default;

	friend auto operator<=>(const SimFloatImpl& A, const float& B)
	{
		return A <=> SimFloatImpl(B);
	};

	friend auto operator<=>(const float& B, const SimFloatImpl& A)
	{
		return SimFloatImpl(B) <=> A;
	};
	friend bool operator==(const SimFloatImpl&, const SimFloatImpl&) = default;
};

// Specialization for Fixed32
template <>
struct SimFloatImpl<Fixed32>
{
	Fixed32 value;

	// Construction
	SimFloatImpl()
		: value(Fixed32::FromRaw(0))
	{
	}

	constexpr SimFloatImpl(const Fixed32& fx)
		: value(fx)
	{
	}

	SimFloatImpl(int32_t i)
		: value(Fixed32::FromInt(i))
	{
	}
#ifdef TNX_FIXED_IMPLICIT_FLOAT
	constexpr SimFloatImpl(float f)
		: value(Fixed32::FromFloat(f))
	{
	}
	constexpr SimFloatImpl(double d)
		: value(Fixed32::FromDouble(d))
	{
	}
#else
	explicit constexpr SimFloatImpl(float f)
		: value(Fixed32::FromFloat(f))
	{
	}

	explicit constexpr SimFloatImpl(double d)
		: value(Fixed32::FromDouble(d))
	{
	}
#endif
	explicit SimFloatImpl(const SimFloatImpl<float>& other)
		: value(Fixed32::FromFloat(other.value))
	{
	}

	// Accessors
	float ToFloat() const { return value.ToFloat(); }
	Fixed32 ToFixed() const { return value; }
	double ToDouble() const { return value.ToDouble(); }

	// Unary operators
	SimFloatImpl operator+() const { return *this; }
	SimFloatImpl operator-() const { return SimFloatImpl(-value); }

	SimFloatImpl& operator++()
	{
		++value;
		return *this;
	}

	SimFloatImpl& operator--()
	{
		--value;
		return *this;
	}

	SimFloatImpl operator++(int)
	{
		SimFloatImpl t = *this;
		++value;
		return t;
	}

	SimFloatImpl operator--(int)
	{
		SimFloatImpl t = *this;
		--value;
		return t;
	}

	// Compound assignment
	SimFloatImpl& operator+=(const SimFloatImpl& rhs)
	{
		value += rhs.value;
		return *this;
	}

	SimFloatImpl& operator-=(const SimFloatImpl& rhs)
	{
		value -= rhs.value;
		return *this;
	}

	SimFloatImpl& operator*=(const SimFloatImpl& rhs)
	{
		value *= rhs.value;
		return *this;
	}

	SimFloatImpl& operator/=(const SimFloatImpl& rhs)
	{
		value /= rhs.value;
		return *this;
	}

	SimFloatImpl& operator+=(int32_t rhs)
	{
		value += rhs;
		return *this;
	}

	SimFloatImpl& operator-=(int32_t rhs)
	{
		value -= rhs;
		return *this;
	}

	SimFloatImpl& operator*=(int32_t rhs)
	{
		value *= rhs;
		return *this;
	}

	SimFloatImpl& operator/=(int32_t rhs)
	{
		value /= rhs;
		return *this;
	}

	// Assignment operators
	SimFloatImpl& operator=(const Fixed32& fx)
	{
		value = fx;
		return *this;
	}

	SimFloatImpl& operator=(float f)
	{
		value = Fixed32::FromFloat(f);
		return *this;
	}

	SimFloatImpl& operator=(double d)
	{
		value = Fixed32::FromDouble(d);
		return *this;
	}

	SimFloatImpl& operator=(int32_t i)
	{
		value = Fixed32::FromInt(i);
		return *this;
	}

	// Comparison
	friend auto operator<=>(const SimFloatImpl&, const SimFloatImpl&) = default;

	friend auto operator<=>(const SimFloatImpl& A, const float& B)
	{
		return A <=> SimFloatImpl(B);
	};

	friend auto operator<=>(const float& B, const SimFloatImpl& A)
	{
		return SimFloatImpl(B) <=> A;
	};
	friend bool operator==(const SimFloatImpl&, const SimFloatImpl&) = default;
};

// Specialization for FixedUnit — unit-range storage (scale = 1<<28, ~8 decimal places).
// Used for quaternion components where Fixed32 (scale = 10,000) loses too much precision.
template <>
struct SimFloatImpl<FixedUnit>
{
	FixedUnit value;

	// Construction
	SimFloatImpl()
		: value(FixedUnit::FromRaw(0))
	{
	}

	constexpr SimFloatImpl(const FixedUnit& fu)
		: value(fu)
	{
	}

	SimFloatImpl(int32_t i)
		: value(FixedUnit::FromInt(i))
	{
	}

#ifdef TNX_FIXED_IMPLICIT_FLOAT
	constexpr SimFloatImpl(float f)
		: value(FixedUnit::FromFloat(f))
	{
	}

	constexpr SimFloatImpl(double d)
		: value(FixedUnit::FromDouble(d))
	{
	}
#else
	explicit constexpr SimFloatImpl(float f)
		: value(FixedUnit::FromFloat(f))
	{
	}

	explicit constexpr SimFloatImpl(double d)
		: value(FixedUnit::FromDouble(d))
	{
	}
#endif

	explicit SimFloatImpl(const SimFloatImpl<float>& other)
		: value(FixedUnit::FromFloat(other.value))
	{
	}

	explicit SimFloatImpl(const SimFloatImpl<Fixed32>& other)
		: value(FixedUnit::FromFixed32(other.value))
	{
	}

	// Accessors
	float ToFloat() const { return value.ToFloat(); }
	FixedUnit ToFixed() const { return value; }
	double ToDouble() const { return value.ToDouble(); }

	// Unary operators
	SimFloatImpl operator+() const { return *this; }
	SimFloatImpl operator-() const { return SimFloatImpl(-value); }

	SimFloatImpl& operator++()
	{
		++value;
		return *this;
	}

	SimFloatImpl& operator--()
	{
		--value;
		return *this;
	}

	SimFloatImpl operator++(int)
	{
		SimFloatImpl t = *this;
		++value;
		return t;
	}

	SimFloatImpl operator--(int)
	{
		SimFloatImpl t = *this;
		--value;
		return t;
	}

	// Compound assignment
	SimFloatImpl& operator+=(const SimFloatImpl& rhs)
	{
		value += rhs.value;
		return *this;
	}

	SimFloatImpl& operator-=(const SimFloatImpl& rhs)
	{
		value -= rhs.value;
		return *this;
	}

	SimFloatImpl& operator*=(const SimFloatImpl& rhs)
	{
		value *= rhs.value;
		return *this;
	}

	SimFloatImpl& operator/=(const SimFloatImpl& rhs)
	{
		value /= rhs.value;
		return *this;
	}

	SimFloatImpl& operator+=(int32_t rhs)
	{
		value += rhs;
		return *this;
	}

	SimFloatImpl& operator-=(int32_t rhs)
	{
		value -= rhs;
		return *this;
	}

	SimFloatImpl& operator*=(int32_t rhs)
	{
		value *= rhs;
		return *this;
	}

	SimFloatImpl& operator/=(int32_t rhs)
	{
		value /= rhs;
		return *this;
	}

	// Cross-type compound assignment: SimUnit *= SimFloat (weight scales unit-range value)
	// Uses the same formula as the binary SimFloat * SimUnit operator.
	SimFloatImpl& operator*=(const SimFloatImpl<Fixed32>& rhs)
	{
		value = FixedUnit::FromRaw(
			static_cast<int32_t>((static_cast<int64_t>(rhs.value.value) * value.value) / Fixed32::Scale64));
		return *this;
	}

	// Assignment operators
	SimFloatImpl& operator=(const FixedUnit& fu)
	{
		value = fu;
		return *this;
	}

	SimFloatImpl& operator=(float f)
	{
		value = FixedUnit::FromFloat(f);
		return *this;
	}

	SimFloatImpl& operator=(double d)
	{
		value = FixedUnit::FromDouble(d);
		return *this;
	}

	SimFloatImpl& operator=(int32_t i)
	{
		value = FixedUnit::FromInt(i);
		return *this;
	}

	// Comparison
	friend auto operator<=>(const SimFloatImpl&, const SimFloatImpl&) = default;

	friend auto operator<=>(const SimFloatImpl& A, const float& B)
	{
		return A <=> SimFloatImpl(B);
	};

	friend auto operator<=>(const float& B, const SimFloatImpl& A)
	{
		return SimFloatImpl(B) <=> A;
	};
	friend bool operator==(const SimFloatImpl&, const SimFloatImpl&) = default;
};

static_assert(sizeof(SimFloatImpl<float>) == 4, "SimFloat<float> must be 4 bytes");
static_assert(sizeof(SimFloatImpl<Fixed32>) == 4, "SimFloat<Fixed32> must be 4 bytes");
static_assert(sizeof(SimFloatImpl<FixedUnit>) == 4, "SimUnit<FixedUnit> must be 4 bytes");

// Binary operators for SimFloatImpl<T>
template <typename T>
FORCE_INLINE SimFloatImpl<T> operator+(SimFloatImpl<T> a, SimFloatImpl<T> b)
{
	a += b;
	return a;
}

template <typename T>
FORCE_INLINE SimFloatImpl<T> operator-(SimFloatImpl<T> a, SimFloatImpl<T> b)
{
	a -= b;
	return a;
}

template <typename T>
FORCE_INLINE SimFloatImpl<T> operator*(SimFloatImpl<T> a, SimFloatImpl<T> b)
{
	a *= b;
	return a;
}

template <typename T>
FORCE_INLINE SimFloatImpl<T> operator/(SimFloatImpl<T> a, SimFloatImpl<T> b)
{
	a /= b;
	return a;
}

// Binary operators for SimFloatImpl<T> op int32_t
template <typename T>
FORCE_INLINE SimFloatImpl<T> operator+(SimFloatImpl<T> a, int32_t b)
{
	a += b;
	return a;
}

template <typename T>
FORCE_INLINE SimFloatImpl<T> operator-(SimFloatImpl<T> a, int32_t b)
{
	a -= b;
	return a;
}

template <typename T>
FORCE_INLINE SimFloatImpl<T> operator*(SimFloatImpl<T> a, int32_t b)
{
	a *= b;
	return a;
}

template <typename T>
FORCE_INLINE SimFloatImpl<T> operator/(SimFloatImpl<T> a, int32_t b)
{
	a /= b;
	return a;
}

// Binary operators for int32_t op SimFloatImpl<T>
template <typename T>
FORCE_INLINE SimFloatImpl<T> operator+(int32_t a, SimFloatImpl<T> b)
{
	return SimFloatImpl<T>(a) + b;
}

template <typename T>
FORCE_INLINE SimFloatImpl<T> operator-(int32_t a, SimFloatImpl<T> b)
{
	return SimFloatImpl<T>(a) - b;
}

template <typename T>
FORCE_INLINE SimFloatImpl<T> operator*(int32_t a, SimFloatImpl<T> b)
{
	return SimFloatImpl<T>(a) * b;
}

template <typename T>
FORCE_INLINE SimFloatImpl<T> operator/(int32_t a, SimFloatImpl<T> b)
{
	return SimFloatImpl<T>(a) / b;
}

// Cross-type operators — SimFloatImpl<FixedUnit> (SimUnit) × SimFloatImpl<Fixed32> (SimFloat)
//
// Two semantically distinct mixed-type products exist and are encoded by argument order:
//
//   SimUnit * SimFloat  →  SimFloat   "apply unit-range factor to a metric value"
//     e.g. quat_component × position_coord — result stays in metric (Fixed32) space
//     raw: (unit.value * metric.value) >> ScaleLog2
//
//   SimFloat * SimUnit  →  SimUnit    "scale a unit-range value by a weight"
//     e.g. blend_weight × quat_component — result stays in unit (FixedUnit) space
//     raw: (weight.value * unit.value) / Fixed32::Scale

// SimUnit * SimFloat → SimFloat: apply unit-range factor to a metric value
//   e.g. quat_component × position_coord → result in Fixed32 (metric) space
//   raw: (unit.value * metric.value) >> ScaleLog2  (uses Fixed32.h: FixedUnit * Fixed32 → Fixed32)
FORCE_INLINE SimFloatImpl<Fixed32> operator*(SimFloatImpl<FixedUnit> a, SimFloatImpl<Fixed32> b)
{
	return SimFloatImpl<Fixed32>(a.value * b.value);
}

// SimFloat * SimUnit → SimUnit: scale a unit-range value by a blend weight
//   e.g. blend_weight × quat_component → result stays in FixedUnit space
//   raw: (weight.value * unit.value) / Fixed32::Scale
FORCE_INLINE SimFloatImpl<FixedUnit> operator*(SimFloatImpl<Fixed32> a, SimFloatImpl<FixedUnit> b)
{
	return SimFloatImpl<FixedUnit>(FixedUnit::FromRaw(
		static_cast<int32_t>((static_cast<int64_t>(a.value.value) * b.value.value) / Fixed32::Scale64)));
}

// Simulation scalar type — float by default, swappable to Fixed32 for
// bit-identical determinism via TNX_DETERMINISTIC build flag.
#ifdef TNX_DETERMINISM
using SimFloat = SimFloatImpl<Fixed32>;
using SimUnit  = SimFloatImpl<FixedUnit>;
#else
using SimFloat = SimFloatImpl<float>;
using SimUnit  = SimFloatImpl<float>;
#endif

// Fast math functions for SimFloat
template <typename T>
FORCE_INLINE SimFloatImpl<T> Sqrt(SimFloatImpl<T> x)
{
	if constexpr (std::is_same_v<T, Fixed32>)
	{
		return SimFloatImpl<T>(FixedSqrt(x.ToFixed()));
	}
	else if constexpr (std::is_same_v<T, FixedUnit>)
	{
		// Integer Newton's method: sqrt(raw / Scale) = isqrt(raw * Scale) / Scale
		if (x.value.value <= 0) return SimFloatImpl<T>(FixedUnit::FromRaw(0));
		int64_t n = static_cast<int64_t>(x.value.value) * FixedUnit::Scale64;
		int64_t r = n;
		while (r > (n / r))
			r = (r + n / r) >> 1;
		return SimFloatImpl<T>(FixedUnit::FromRaw(static_cast<int32_t>(r)));
	}
	else
	{
		return SimFloatImpl<T>(std::sqrt(x.ToFloat()));
	}
}

template <typename T>
FORCE_INLINE SimFloatImpl<T> Rsqrt(SimFloatImpl<T> x)
{
	if constexpr (std::is_same_v<T, Fixed32>)
	{
		// 1/sqrt(x) = Scale² / FixedSqrt(x).raw — fully deterministic integer path.
		// Guard against zero input (returns max representable value).
		const Fixed32 s = FixedSqrt(x.ToFixed());
		if (s.value == 0) return SimFloatImpl<T>(Fixed32::FromRaw(0x7FFFFFFF));
		return SimFloatImpl<T>(Fixed32::FromRaw(
			static_cast<int32_t>((Fixed32::Scale64 * Fixed32::Scale64) / static_cast<int64_t>(s.value))));
	}
	else
	{
		// float: use SSE rsqrt + Newton-Raphson
		__m128 val    = _mm_set_ss(x.ToFloat());
		__m128 approx = _mm_rsqrt_ss(val);
		// One Newton-Raphson iteration: r2 = approx * (1.5 - 0.5 * x * approx * approx)
		__m128 half      = _mm_set_ss(0.5f);
		__m128 threeHalf = _mm_set_ss(1.5f);
		__m128 xhalf     = _mm_mul_ss(val, half);
		__m128 asq       = _mm_mul_ss(approx, approx);
		__m128 tmp       = _mm_mul_ss(xhalf, asq);
		__m128 refined   = _mm_mul_ss(approx, _mm_sub_ss(threeHalf, tmp));
		float result;
		_mm_store_ss(&result, refined);
		return SimFloatImpl<T>(result);
	}
}

template <typename T>
FORCE_INLINE SimFloatImpl<T> FastSin(SimFloatImpl<T> x)
{
	if constexpr (std::is_same_v<T, Fixed32>)
	{
		const FixedUnit r = FixedSin(x.ToFixed());
		return SimFloatImpl<T>(Fixed32::FromRaw(
			static_cast<int32_t>((static_cast<int64_t>(r.value) * Fixed32::Scale64) >> FixedUnit::ScaleLog2)));
	}
	else
		return SimFloatImpl<T>(std::sin(x.value));
}

template <typename T>
FORCE_INLINE SimFloatImpl<T> FastCos(SimFloatImpl<T> x)
{
	if constexpr (std::is_same_v<T, Fixed32>)
	{
		const FixedUnit r = FixedCos(x.ToFixed());
		return SimFloatImpl<T>(Fixed32::FromRaw(
			static_cast<int32_t>((static_cast<int64_t>(r.value) * Fixed32::Scale64) >> FixedUnit::ScaleLog2)));
	}
	else
		return SimFloatImpl<T>(std::cos(x.value));
}

template <typename T>
FORCE_INLINE SimFloatImpl<T> FastTan(SimFloatImpl<T> x)
{
	if constexpr (std::is_same_v<T, Fixed32>)
	{
		const auto s = FastSin(x);
		const auto c = FastCos(x);
		if (c.ToFixed().value == 0) return SimFloatImpl<T>(Fixed32::FromInt(99999));
		return s / c;
	}
	else
		return SimFloatImpl<T>(std::tan(x.value));
}

template <typename T>
FORCE_INLINE SimFloatImpl<T> Fmod(SimFloatImpl<T> a, SimFloatImpl<T> b)
{
	if constexpr (std::is_same_v<T, Fixed32>)
		return SimFloatImpl<T>(Fixed32::FromRaw(a.value.value % b.value.value));
	else
		return SimFloatImpl<T>(std::fmod(a.value, b.value));
}
