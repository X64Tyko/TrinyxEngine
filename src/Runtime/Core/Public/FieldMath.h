#pragma once

#include "FieldProxy.h"
#include <cmath>

// FieldMath — per-field utility operations on FieldProxy.
//
// All functions return values (float or __m256). Assign the result back
// to a FieldProxy to write it — the proxy's operator= handles dirty bits.
//
//   velocity.vX = FieldMath::Clamp(velocity.vX, -maxSpeed, maxSpeed);
//   health = FieldMath::Max(health, 0.0f);
//   field = FieldMath::Lerp(fieldA, fieldB, 0.5f);

namespace FieldMath
{
	// ── Read helper (extracts value without marking dirty) ──────────────────

	template <FieldWidth WIDTH>
	FORCE_INLINE auto Read(const FloatProxy<WIDTH>& f)
	{
		if constexpr (WIDTH == FieldWidth::Scalar) return f.WriteArray[f.index];
		else return SIMDTraits < float, WIDTH > ::load(&f.WriteArray[f.index]);
	}

	template <FieldWidth WIDTH>
	FORCE_INLINE auto Read(const IntProxy<WIDTH>& f)
	{
		if constexpr (WIDTH == FieldWidth::Scalar) return f.WriteArray[f.index];
		else return SIMDTraits<int32_t, WIDTH>::load(&f.WriteArray[f.index]);
	}

	template <FieldWidth WIDTH>
	FORCE_INLINE auto Read(const UIntProxy<WIDTH>& f)
	{
		if constexpr (WIDTH == FieldWidth::Scalar) return f.WriteArray[f.index];
		else return SIMDTraits<uint32_t, WIDTH>::load(&f.WriteArray[f.index]);
	}

	// ── Clamp ───────────────────────────────────────────────────────────────

	template <FieldWidth WIDTH>
	FORCE_INLINE auto Clamp(const FloatProxy<WIDTH>& f, float lo, float hi)
	{
		using T = SIMDTraits<float, WIDTH>;
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			float v = f.WriteArray[f.index];
			return v < lo ? lo : (v > hi ? hi : v);
		}
		else
		{
			auto v = T::load(&f.WriteArray[f.index]);
			return T::min(T::max(v, T::set1(lo)), T::set1(hi));
		}
	}

	template <FieldWidth WIDTH>
	FORCE_INLINE auto Clamp(const IntProxy<WIDTH>& f, int32_t lo, int32_t hi)
	{
		using T = SIMDTraits<int32_t, WIDTH>;
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			int32_t v = f.WriteArray[f.index];
			return v < lo ? lo : (v > hi ? hi : v);
		}
		else
		{
			auto v = T::load(&f.WriteArray[f.index]);
			return T::min(T::max(v, T::set1(lo)), T::set1(hi));
		}
	}

	// ── Min / Max ───────────────────────────────────────────────────────────

	template <FieldWidth WIDTH>
	FORCE_INLINE auto Min(const FloatProxy<WIDTH>& f, float val)
	{
		using T = SIMDTraits<float, WIDTH>;
		if constexpr (WIDTH == FieldWidth::Scalar) return f.WriteArray[f.index] < val ? f.WriteArray[f.index] : val;
		else return T::min(T::load(&f.WriteArray[f.index]), T::set1(val));
	}

	template <FieldWidth WIDTH>
	FORCE_INLINE auto Max(const FloatProxy<WIDTH>& f, float val)
	{
		using T = SIMDTraits<float, WIDTH>;
		if constexpr (WIDTH == FieldWidth::Scalar) return f.WriteArray[f.index] > val ? f.WriteArray[f.index] : val;
		else return T::max(T::load(&f.WriteArray[f.index]), T::set1(val));
	}

	// Min/Max between two proxies
	template <FieldWidth WIDTH>
	FORCE_INLINE auto Min(const FloatProxy<WIDTH>& a, const FloatProxy<WIDTH>& b)
	{
		using T = SIMDTraits<float, WIDTH>;
		if constexpr (WIDTH == FieldWidth::Scalar) return a.WriteArray[a.index] < b.WriteArray[b.index] ? a.WriteArray[a.index] : b.WriteArray[b.index];
		else return T::min(T::load(&a.WriteArray[a.index]), T::load(&b.WriteArray[b.index]));
	}

	template <FieldWidth WIDTH>
	FORCE_INLINE auto Max(const FloatProxy<WIDTH>& a, const FloatProxy<WIDTH>& b)
	{
		using T = SIMDTraits<float, WIDTH>;
		if constexpr (WIDTH == FieldWidth::Scalar) return a.WriteArray[a.index] > b.WriteArray[b.index] ? a.WriteArray[a.index] : b.WriteArray[b.index];
		else return T::max(T::load(&a.WriteArray[a.index]), T::load(&b.WriteArray[b.index]));
	}

	// ── Abs ─────────────────────────────────────────────────────────────────

	template <FieldWidth WIDTH>
	FORCE_INLINE auto Abs(const FloatProxy<WIDTH>& f)
	{
		using T = SIMDTraits<float, WIDTH>;
		if constexpr (WIDTH == FieldWidth::Scalar) return std::abs(f.WriteArray[f.index]);
		else return T::abs(T::load(&f.WriteArray[f.index]));
	}

	template <FieldWidth WIDTH>
	FORCE_INLINE auto Abs(const IntProxy<WIDTH>& f)
	{
		using T = SIMDTraits<int32_t, WIDTH>;
		if constexpr (WIDTH == FieldWidth::Scalar) return std::abs(f.WriteArray[f.index]);
		else return T::abs(T::load(&f.WriteArray[f.index]));
	}

	// ── Lerp ────────────────────────────────────────────────────────────────
	// result = a + t * (b - a)

	template <FieldWidth WIDTH>
	FORCE_INLINE auto Lerp(const FloatProxy<WIDTH>& a, const FloatProxy<WIDTH>& b, float t)
	{
		using T = SIMDTraits<float, WIDTH>;
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			float va = a.WriteArray[a.index];
			float vb = b.WriteArray[b.index];
			return va + t * (vb - va);
		}
		else
		{
			auto va = T::load(&a.WriteArray[a.index]);
			auto vb = T::load(&b.WriteArray[b.index]);
			auto vt = T::set1(t);
			return T::add(va, T::mul(vt, T::sub(vb, va)));
		}
	}

	// Lerp between proxy and scalar target
	template <FieldWidth WIDTH>
	FORCE_INLINE auto Lerp(const FloatProxy<WIDTH>& a, float target, float t)
	{
		using T = SIMDTraits<float, WIDTH>;
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			float va = a.WriteArray[a.index];
			return va + t * (target - va);
		}
		else
		{
			auto va = T::load(&a.WriteArray[a.index]);
			auto vt = T::set1(t);
			auto vb = T::set1(target);
			return T::add(va, T::mul(vt, T::sub(vb, va)));
		}
	}

	// ── Step / SmoothStep ───────────────────────────────────────────────────

	// Returns 0 if val < edge, 1 otherwise
	template <FieldWidth WIDTH>
	FORCE_INLINE auto Step(const FloatProxy<WIDTH>& f, float edge)
	{
		using T = SIMDTraits<float, WIDTH>;
		if constexpr (WIDTH == FieldWidth::Scalar) return f.WriteArray[f.index] >= edge ? 1.0f : 0.0f;
		else
		{
			auto v    = T::load(&f.WriteArray[f.index]);
			auto e    = T::set1(edge);
			auto mask = _mm256_cmp_ps(v, e, _CMP_GE_OQ);
			return _mm256_and_ps(mask, T::set1(1.0f));
		}
	}

	// Hermite smoothstep: 0 below lo, 1 above hi, smooth cubic in between
	template <FieldWidth WIDTH>
	FORCE_INLINE auto SmoothStep(const FloatProxy<WIDTH>& f, float lo, float hi)
	{
		using T = SIMDTraits<float, WIDTH>;
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			float v = f.WriteArray[f.index];
			float t = (v - lo) / (hi - lo);
			t       = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
			return t * t * (3.0f - 2.0f * t);
		}
		else
		{
			auto v         = T::load(&f.WriteArray[f.index]);
			auto zero      = T::set1(0.0f);
			auto one       = T::set1(1.0f);
			auto range_inv = T::set1(1.0f / (hi - lo));
			auto t         = T::mul(T::sub(v, T::set1(lo)), range_inv);
			t              = T::min(T::max(t, zero), one); // clamp [0,1]
			// t * t * (3 - 2t)
			return T::mul(T::mul(t, t), T::sub(T::set1(3.0f), T::mul(T::set1(2.0f), t)));
		}
	}
} // namespace FieldMath