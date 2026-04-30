#pragma once

#include <cstdint>
#include <compare>

#include "Globals.h" // FixedUnitsPerMeter

// Fixed32 — 32-bit fixed-point scalar for deterministic simulation.
//
// Encoding: 1 unit = 0.1mm. 10,000 units = 1m. int32 range = ±214km.
// Layout:   plain int32_t — bit-identical across platforms.
// Multiply: int64 intermediate / Scale (decimal, NOT a power-of-2 shift).
//
// Implicit float conversion is gated behind TNX_FIXED_IMPLICIT_FLOAT so a strict
// build can compile-check that no accidental float ↔ Fixed32 coercions remain in
// gameplay code. The explicit FromFloat / ToFloat path is always available for
// boundary conversions (Jolt bridge, render upload, debug printing, scene I/O).
struct Fixed32
{
	int32_t value;

	static constexpr int32_t Scale   = FixedUnitsPerMeter;
	static constexpr int64_t Scale64 = static_cast<int64_t>(Scale);

	// --- Construction --------------------------------------------------------

	constexpr Fixed32()
		: value(0)
	{
	}

	constexpr Fixed32(const int32_t i)
		: value(i * Scale)
	{
	}

	static constexpr Fixed32 FromRaw(int32_t raw)
	{
		Fixed32 r;
		r.value = raw;
		return r;
	}

	static constexpr Fixed32 FromInt(int32_t i) { return FromRaw(i * Scale); }
	static constexpr Fixed32 FromFloat(float f) { return FromRaw(static_cast<int32_t>(f * static_cast<float>(Scale))); }
	static constexpr Fixed32 FromDouble(double f) { return FromRaw(static_cast<int32_t>(f * static_cast<double>(Scale))); }

	// --- Always-available explicit accessors --------------------------------

	constexpr int32_t Raw() const { return value; }
	constexpr float ToFloat() const { return static_cast<float>(value) / static_cast<float>(Scale); }
	constexpr double ToDouble() const { return static_cast<double>(value) / static_cast<double>(Scale); }

	// --- Float interop (gated) ---------------------------------------------
	//
	// When TNX_FIXED_IMPLICIT_FLOAT is defined, Fixed32 silently converts to/from
	// float and double. Disable it for a build where any unintended float coercion
	// from gameplay code becomes a compile error.
#ifdef TNX_FIXED_IMPLICIT_FLOAT
	constexpr Fixed32(float f)
		: value(static_cast<int32_t>(f * static_cast<float>(Scale)))
	{
	}
	constexpr Fixed32(double f)
		: value(static_cast<int32_t>(f * static_cast<double>(Scale)))
	{
	}

	constexpr operator float() const { return ToFloat(); }
	constexpr operator double() const { return ToDouble(); }
#endif

	// --- Unary --------------------------------------------------------------

	constexpr Fixed32 operator+() const { return *this; }
	constexpr Fixed32 operator-() const { return FromRaw(-value); }

	constexpr Fixed32& operator++()
	{
		value += Scale;
		return *this;
	}

	constexpr Fixed32& operator--()
	{
		value -= Scale;
		return *this;
	}

	constexpr Fixed32 operator++(int)
	{
		Fixed32 t = *this;
		value     += Scale;
		return t;
	}

	constexpr Fixed32 operator--(int)
	{
		Fixed32 t = *this;
		value     -= Scale;
		return t;
	}

	// --- Compound assignment with Fixed32 -----------------------------------

	constexpr Fixed32& operator+=(Fixed32 rhs)
	{
		value += rhs.value;
		return *this;
	}

	constexpr Fixed32& operator-=(Fixed32 rhs)
	{
		value -= rhs.value;
		return *this;
	}

	constexpr Fixed32& operator*=(Fixed32 rhs)
	{
		value = static_cast<int32_t>((static_cast<int64_t>(value) * rhs.value) / Scale64);
		return *this;
	}

	constexpr Fixed32& operator/=(Fixed32 rhs)
	{
		value = static_cast<int32_t>((static_cast<int64_t>(value) * Scale64) / rhs.value);
		return *this;
	}

	constexpr Fixed32& operator%=(Fixed32 rhs)
	{
		value %= rhs.value;
		return *this;
	}

	// --- Compound assignment with int (matches float-semantics) -------------
	// + and - treat the int as a value-with-units (promotion to Fixed32).
	// * and / treat the int as a unitless scalar — the fast path, no rescale.

	constexpr Fixed32& operator+=(int32_t rhs)
	{
		value += rhs * Scale;
		return *this;
	}

	constexpr Fixed32& operator-=(int32_t rhs)
	{
		value -= rhs * Scale;
		return *this;
	}

	constexpr Fixed32& operator*=(int32_t rhs)
	{
		value *= rhs;
		return *this;
	}

	constexpr Fixed32& operator/=(int32_t rhs)
	{
		value /= rhs;
		return *this;
	}

	// --- Comparison ---------------------------------------------------------

	friend constexpr auto operator<=>(Fixed32 a, Fixed32 b) = default;
	friend constexpr bool operator==(Fixed32 a, Fixed32 b)  = default;
};

// FixedUnit — unit-range fixed-point for trig output and direction components.
//
// Encoding: Scale = 1<<20 (1,048,576). 1.0 = Scale. Range ≈ [-2, +2].
// Used as the return type of FixedSin / FixedCos so dot products with Fixed32
// positions stay exact: (int64_t(pos.value) * unit.value) >> 20.
struct FixedUnit : Fixed32
{
	static constexpr int32_t Scale   = FixedUnitsPerRadian; // 1,048,576 = 1<<20
	static constexpr int64_t Scale64 = static_cast<int64_t>(Scale);

	// --- Construction --------------------------------------------------------

	constexpr FixedUnit() = default;

	constexpr FixedUnit(int32_t i) // shadows Fixed32(int32_t) — uses FixedUnit::Scale
	{
		value = i * Scale;
	}

	static constexpr FixedUnit FromRaw(int32_t raw)
	{
		FixedUnit r;
		r.value = raw;
		return r;
	}

	static constexpr FixedUnit FromFloat(float f)
	{
		return FromRaw(static_cast<int32_t>(f * static_cast<float>(Scale)));
	}

	static constexpr FixedUnit FromDouble(double f)
	{
		return FromRaw(static_cast<int32_t>(f * static_cast<double>(Scale)));
	}

	// --- Accessors (override to use FixedUnit::Scale) -----------------------

	constexpr float ToFloat() const { return static_cast<float>(value) / static_cast<float>(Scale); }
	constexpr double ToDouble() const { return static_cast<double>(value) / static_cast<double>(Scale); }

	// --- Unary --------------------------------------------------------------

	constexpr FixedUnit operator+() const { return *this; }
	constexpr FixedUnit operator-() const { return FromRaw(-value); }

	// --- Compound assignment — FixedUnit op FixedUnit -----------------------

	constexpr FixedUnit& operator+=(FixedUnit rhs)
	{
		value += rhs.value;
		return *this;
	}

	constexpr FixedUnit& operator-=(FixedUnit rhs)
	{
		value -= rhs.value;
		return *this;
	}

	constexpr FixedUnit& operator*=(FixedUnit rhs)
	{
		value = static_cast<int32_t>((static_cast<int64_t>(value) * rhs.value) / Scale64);
		return *this;
	}

	constexpr FixedUnit& operator/=(FixedUnit rhs)
	{
		value = static_cast<int32_t>((static_cast<int64_t>(value) * Scale64) / rhs.value);
		return *this;
	}

	// --- Compound assignment — unitless int scalar --------------------------
	// + and - treat int as a whole unit (1 → Scale). * and / are raw scalars.

	constexpr FixedUnit& operator+=(int32_t rhs)
	{
		value += rhs * Scale;
		return *this;
	}

	constexpr FixedUnit& operator-=(int32_t rhs)
	{
		value -= rhs * Scale;
		return *this;
	}

	constexpr FixedUnit& operator*=(int32_t rhs)
	{
		value *= rhs;
		return *this;
	}

	constexpr FixedUnit& operator/=(int32_t rhs)
	{
		value /= rhs;
		return *this;
	}

	// --- Comparison ---------------------------------------------------------

	friend constexpr auto operator<=>(FixedUnit a, FixedUnit b) = default;
	friend constexpr bool operator==(FixedUnit a, FixedUnit b)  = default;
};

// --- Binary arithmetic — Fixed32 op Fixed32 ---------------------------------

constexpr Fixed32 operator+(Fixed32 a, Fixed32 b) { return Fixed32::FromRaw(a.value + b.value); }
constexpr Fixed32 operator-(Fixed32 a, Fixed32 b) { return Fixed32::FromRaw(a.value - b.value); }

constexpr Fixed32 operator*(Fixed32 a, Fixed32 b)
{
	return Fixed32::FromRaw(static_cast<int32_t>((static_cast<int64_t>(a.value) * b.value) / Fixed32::Scale64));
}

constexpr Fixed32 operator/(Fixed32 a, Fixed32 b)
{
	return Fixed32::FromRaw(static_cast<int32_t>((static_cast<int64_t>(a.value) * Fixed32::Scale64) / b.value));
}

constexpr Fixed32 operator%(Fixed32 a, Fixed32 b) { return Fixed32::FromRaw(a.value % b.value); }

// --- Binary arithmetic — Fixed32 op int / int op Fixed32 --------------------
// Mirrors float ↔ int semantics: + and - promote int to "value with units",
// * and / treat int as a unitless scalar so the multiply is single-precision fast.

constexpr Fixed32 operator+(Fixed32 a, int32_t b) { return Fixed32::FromRaw(a.value + b * Fixed32::Scale); }
constexpr Fixed32 operator+(int32_t a, Fixed32 b) { return Fixed32::FromRaw(a * Fixed32::Scale + b.value); }

constexpr Fixed32 operator-(Fixed32 a, int32_t b) { return Fixed32::FromRaw(a.value - b * Fixed32::Scale); }
constexpr Fixed32 operator-(int32_t a, Fixed32 b) { return Fixed32::FromRaw(a * Fixed32::Scale - b.value); }

constexpr Fixed32 operator*(Fixed32 a, int32_t b) { return Fixed32::FromRaw(a.value * b); }
constexpr Fixed32 operator*(int32_t a, Fixed32 b) { return Fixed32::FromRaw(a * b.value); }

constexpr Fixed32 operator/(Fixed32 a, int32_t b) { return Fixed32::FromRaw(a.value / b); }

constexpr Fixed32 operator/(int32_t a, Fixed32 b)
{
	return Fixed32::FromRaw(static_cast<int32_t>((static_cast<int64_t>(a) * Fixed32::Scale64 * Fixed32::Scale64) / b.value));
}

// --- Float / double binary operators (gated) --------------------------------
//
// Defining these inside the gate means a strict (TNX_FIXED_IMPLICIT_FLOAT off)
// build will reject expressions like `fixedVal + 1.5f` with a clear template /
// overload-resolution error rather than silently converting. Use FromFloat /
// ToFloat at the boundary instead.
#ifdef TNX_FIXED_IMPLICIT_FLOAT
constexpr Fixed32 operator+(Fixed32 a, float b) { return a + Fixed32::FromFloat(b); }
constexpr Fixed32 operator+(float a, Fixed32 b) { return Fixed32::FromFloat(a) + b; }
constexpr Fixed32 operator-(Fixed32 a, float b) { return a - Fixed32::FromFloat(b); }
constexpr Fixed32 operator-(float a, Fixed32 b) { return Fixed32::FromFloat(a) - b; }
constexpr Fixed32 operator*(Fixed32 a, float b) { return a * Fixed32::FromFloat(b); }
constexpr Fixed32 operator*(float a, Fixed32 b) { return Fixed32::FromFloat(a) * b; }
constexpr Fixed32 operator/(Fixed32 a, float b) { return a / Fixed32::FromFloat(b); }
constexpr Fixed32 operator/(float a, Fixed32 b) { return Fixed32::FromFloat(a) / b; }

constexpr Fixed32 operator+(Fixed32 a, double b) { return a + Fixed32::FromDouble(b); }
constexpr Fixed32 operator+(double a, Fixed32 b) { return Fixed32::FromDouble(a) + b; }
constexpr Fixed32 operator-(Fixed32 a, double b) { return a - Fixed32::FromDouble(b); }
constexpr Fixed32 operator-(double a, Fixed32 b) { return Fixed32::FromDouble(a) - b; }
constexpr Fixed32 operator*(Fixed32 a, double b) { return a * Fixed32::FromDouble(b); }
constexpr Fixed32 operator*(double a, Fixed32 b) { return Fixed32::FromDouble(a) * b; }
constexpr Fixed32 operator/(Fixed32 a, double b) { return a / Fixed32::FromDouble(b); }
constexpr Fixed32 operator/(double a, Fixed32 b) { return Fixed32::FromDouble(a) / b; }
#endif

// --- Binary arithmetic — FixedUnit op FixedUnit ---------------------------------

constexpr FixedUnit operator+(FixedUnit a, FixedUnit b) { return FixedUnit::FromRaw(a.value + b.value); }
constexpr FixedUnit operator-(FixedUnit a, FixedUnit b) { return FixedUnit::FromRaw(a.value - b.value); }

constexpr FixedUnit operator*(FixedUnit a, FixedUnit b)
{
	return FixedUnit::FromRaw(static_cast<int32_t>((static_cast<int64_t>(a.value) * b.value) / FixedUnit::Scale64));
}

constexpr FixedUnit operator/(FixedUnit a, FixedUnit b)
{
	return FixedUnit::FromRaw(static_cast<int32_t>((static_cast<int64_t>(a.value) * FixedUnit::Scale64) / b.value));
}

// --- Binary arithmetic — FixedUnit op int / int op FixedUnit --------------------
// + and - treat int as a whole unit (1 = Scale). * and / are unitless scalars.

constexpr FixedUnit operator+(FixedUnit a, int32_t b) { return FixedUnit::FromRaw(a.value + b * FixedUnit::Scale); }
constexpr FixedUnit operator+(int32_t a, FixedUnit b) { return FixedUnit::FromRaw(a * FixedUnit::Scale + b.value); }

constexpr FixedUnit operator-(FixedUnit a, int32_t b) { return FixedUnit::FromRaw(a.value - b * FixedUnit::Scale); }
constexpr FixedUnit operator-(int32_t a, FixedUnit b) { return FixedUnit::FromRaw(a * FixedUnit::Scale - b.value); }

constexpr FixedUnit operator*(FixedUnit a, int32_t b) { return FixedUnit::FromRaw(a.value * b); }
constexpr FixedUnit operator*(int32_t a, FixedUnit b) { return FixedUnit::FromRaw(a * b.value); }

constexpr FixedUnit operator/(FixedUnit a, int32_t b) { return FixedUnit::FromRaw(a.value / b); }

// --- Cross-type: Fixed32 scaled by FixedUnit ------------------------------------
// FixedUnit::Scale is a power of 2 (1<<20), so division is a pure right shift.
// Used for: pos * sin(angle), velocity * cos(heading), etc.

constexpr Fixed32 operator*(Fixed32 a, FixedUnit b)
{
	return Fixed32::FromRaw(static_cast<int32_t>((static_cast<int64_t>(a.value) * b.value) >> 20));
}

constexpr Fixed32 operator*(FixedUnit a, Fixed32 b)
{
	return Fixed32::FromRaw(static_cast<int32_t>((static_cast<int64_t>(a.value) * b.value) >> 20));
}

// --- Fast square root for Fixed32 (integer Newton) -------------------------
// Deterministic, no floating-point, fast convergence (≤3 iterations).
// Returns the fixed-point result scaled by the same Scale.
// Public domain – integer Newton–Raphson
constexpr Fixed32 FixedSqrt(Fixed32 x)
{
	if (x.value <= 0) return Fixed32::FromRaw(0);

	// Compute integer sqrt of (raw * Scale) — result is raw of sqrt(x).
	int64_t n  = static_cast<int64_t>(x.value) * Fixed32::Scale;
	int64_t r  = n; // initial guess
	// Newton's method for integer sqrt: r = (r + n/r) / 2
	while (r > (n / r))
	{
		r  = (r + n / r) >> 1;
	}
	return Fixed32::FromRaw(static_cast<int32_t>(r));
}
