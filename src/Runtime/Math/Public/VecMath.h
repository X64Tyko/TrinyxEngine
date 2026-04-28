#pragma once

#include "FieldProxy.h"
#include "FastTrig.h"
#include <cmath>

// VecMath — vector operations on FieldProxy<float, WIDTH> groups.
//
// Same pattern as QuatMath: loads fields into locals before cross-dependent
// writes, correct at every FieldWidth (Scalar, Wide, WideMask).
//
// Entity authors use the accessor structs (Vec2Accessor, Vec3Accessor,
// Vec4Accessor) embedded in components — not the namespace functions directly.
//
// Usage:
//   transform.Position += velocity.Vel * fdt;
//   transform.Position.Normalize();
//   float len = transform.Position.Length();

namespace VecMath
{
	// ─── Intermediate Storage ───────────────────────────────────────────────
	// float in Scalar, __m256 in Wide/WideMask. Returned by binary ops on
	// accessors so that compound expressions work without touching SoA mid-chain.

	template <int N, FieldWidth WIDTH>
	struct VecLocal
	{
		using Val = std::conditional_t<WIDTH == FieldWidth::Scalar, float, __m256>;
		Val v[N];

		FORCE_INLINE Val& operator[](int i) { return v[i]; }
		FORCE_INLINE const Val& operator[](int i) const { return v[i]; }

		// ── VecLocal + VecLocal ──
		FORCE_INLINE VecLocal operator+(const VecLocal& o) const
		{
			VecLocal r;
			if constexpr (WIDTH == FieldWidth::Scalar) for (int i = 0; i < N; ++i) r.v[i] = v[i] + o.v[i];
			else
			{
				using T = SIMDTraits<float, WIDTH>;
				for (int i = 0; i < N; ++i) r.v[i] = T::add(v[i], o.v[i]);
			}
			return r;
		}

		FORCE_INLINE VecLocal operator-(const VecLocal& o) const
		{
			VecLocal r;
			if constexpr (WIDTH == FieldWidth::Scalar) for (int i = 0; i < N; ++i) r.v[i] = v[i] - o.v[i];
			else
			{
				using T = SIMDTraits<float, WIDTH>;
				for (int i = 0; i < N; ++i) r.v[i] = T::sub(v[i], o.v[i]);
			}
			return r;
		}

		// Component-wise multiply (VecLocal * VecLocal)
		FORCE_INLINE VecLocal operator*(const VecLocal& o) const
		{
			VecLocal r;
			if constexpr (WIDTH == FieldWidth::Scalar) for (int i = 0; i < N; ++i) r.v[i] = v[i] * o.v[i];
			else
			{
				using T = SIMDTraits<float, WIDTH>;
				for (int i = 0; i < N; ++i) r.v[i] = T::mul(v[i], o.v[i]);
			}
			return r;
		}

		// ── VecLocal * scalar ──
		FORCE_INLINE VecLocal operator*(float s) const
		{
			VecLocal r;
			if constexpr (WIDTH == FieldWidth::Scalar) for (int i = 0; i < N; ++i) r.v[i] = v[i] * s;
			else
			{
				using T         = SIMDTraits<float, WIDTH>;
				const __m256 sv = T::set1(s);
				for (int i = 0; i < N; ++i) r.v[i] = T::mul(v[i], sv);
			}
			return r;
		}

		FORCE_INLINE friend VecLocal operator*(float s, const VecLocal& vec) { return vec * s; }

		FORCE_INLINE VecLocal operator/(float s) const
		{
			VecLocal r;
			if constexpr (WIDTH == FieldWidth::Scalar) for (int i = 0; i < N; ++i) r.v[i] = v[i] / s;
			else
			{
				using T         = SIMDTraits<float, WIDTH>;
				const __m256 sv = T::set1(s);
				for (int i = 0; i < N; ++i) r.v[i] = T::div(v[i], sv);
			}
			return r;
		}

		FORCE_INLINE VecLocal operator-() const
		{
			VecLocal r;
			if constexpr (WIDTH == FieldWidth::Scalar) for (int i = 0; i < N; ++i) r.v[i] = -v[i];
			else
			{
				using T           = SIMDTraits<float, WIDTH>;
				const __m256 zero = T::set1(0.0f);
				for (int i = 0; i < N; ++i) r.v[i] = T::sub(zero, v[i]);
			}
			return r;
		}
	};

	// ─── Load helpers ───────────────────────────────────────────────────────

	template <FieldWidth WIDTH>
	FORCE_INLINE VecLocal<2, WIDTH> Load2(
		const FloatProxy<WIDTH>& x, const FloatProxy<WIDTH>& y)
	{
		VecLocal<2, WIDTH> r;
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			r.v[0] = x.WriteArray[x.index];
			r.v[1] = y.WriteArray[y.index];
		}
		else
		{
			using T = SIMDTraits<float, WIDTH>;
			r.v[0]  = T::load(&x.WriteArray[x.index]);
			r.v[1]  = T::load(&y.WriteArray[y.index]);
		}
		return r;
	}

	template <FieldWidth WIDTH>
	FORCE_INLINE VecLocal<3, WIDTH> Load3(
		const FloatProxy<WIDTH>& x, const FloatProxy<WIDTH>& y, const FloatProxy<WIDTH>& z)
	{
		VecLocal<3, WIDTH> r;
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			r.v[0] = x.WriteArray[x.index];
			r.v[1] = y.WriteArray[y.index];
			r.v[2] = z.WriteArray[z.index];
		}
		else
		{
			using T = SIMDTraits<float, WIDTH>;
			r.v[0]  = T::load(&x.WriteArray[x.index]);
			r.v[1]  = T::load(&y.WriteArray[y.index]);
			r.v[2]  = T::load(&z.WriteArray[z.index]);
		}
		return r;
	}

	template <FieldWidth WIDTH>
	FORCE_INLINE VecLocal<4, WIDTH> Load4(
		const FloatProxy<WIDTH>& x, const FloatProxy<WIDTH>& y,
		const FloatProxy<WIDTH>& z, const FloatProxy<WIDTH>& w)
	{
		VecLocal<4, WIDTH> r;
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			r.v[0] = x.WriteArray[x.index];
			r.v[1] = y.WriteArray[y.index];
			r.v[2] = z.WriteArray[z.index];
			r.v[3] = w.WriteArray[w.index];
		}
		else
		{
			using T = SIMDTraits<float, WIDTH>;
			r.v[0]  = T::load(&x.WriteArray[x.index]);
			r.v[1]  = T::load(&y.WriteArray[y.index]);
			r.v[2]  = T::load(&z.WriteArray[z.index]);
			r.v[3]  = T::load(&w.WriteArray[w.index]);
		}
		return r;
	}

	// ─── Dot products ───────────────────────────────────────────────────────

	template <FieldWidth WIDTH>
	FORCE_INLINE typename VecLocal<2, WIDTH>::Val Dot2(
		const VecLocal<2, WIDTH>& a, const VecLocal<2, WIDTH>& b)
	{
		if constexpr (WIDTH == FieldWidth::Scalar) return a.v[0] * b.v[0] + a.v[1] * b.v[1];
		else
		{
			using T = SIMDTraits<float, WIDTH>;
			return T::add(T::mul(a.v[0], b.v[0]), T::mul(a.v[1], b.v[1]));
		}
	}

	template <FieldWidth WIDTH>
	FORCE_INLINE typename VecLocal<3, WIDTH>::Val Dot3(
		const VecLocal<3, WIDTH>& a, const VecLocal<3, WIDTH>& b)
	{
		if constexpr (WIDTH == FieldWidth::Scalar) return a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2];
		else
		{
			using T = SIMDTraits<float, WIDTH>;
			return T::add(T::add(T::mul(a.v[0], b.v[0]), T::mul(a.v[1], b.v[1])),
						  T::mul(a.v[2], b.v[2]));
		}
	}

	template <FieldWidth WIDTH>
	FORCE_INLINE typename VecLocal<4, WIDTH>::Val Dot4(
		const VecLocal<4, WIDTH>& a, const VecLocal<4, WIDTH>& b)
	{
		if constexpr (WIDTH == FieldWidth::Scalar) return a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2] + a.v[3] * b.v[3];
		else
		{
			using T = SIMDTraits<float, WIDTH>;
			return T::add(T::add(T::mul(a.v[0], b.v[0]), T::mul(a.v[1], b.v[1])),
						  T::add(T::mul(a.v[2], b.v[2]), T::mul(a.v[3], b.v[3])));
		}
	}

	// ─── Cross product (Vec3 only) ──────────────────────────────────────────

	template <FieldWidth WIDTH>
	FORCE_INLINE VecLocal<3, WIDTH> Cross3(
		const VecLocal<3, WIDTH>& a, const VecLocal<3, WIDTH>& b)
	{
		VecLocal<3, WIDTH> r;
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			r.v[0] = a.v[1] * b.v[2] - a.v[2] * b.v[1];
			r.v[1] = a.v[2] * b.v[0] - a.v[0] * b.v[2];
			r.v[2] = a.v[0] * b.v[1] - a.v[1] * b.v[0];
		}
		else
		{
			using T = SIMDTraits<float, WIDTH>;
			r.v[0]  = T::sub(T::mul(a.v[1], b.v[2]), T::mul(a.v[2], b.v[1]));
			r.v[1]  = T::sub(T::mul(a.v[2], b.v[0]), T::mul(a.v[0], b.v[2]));
			r.v[2]  = T::sub(T::mul(a.v[0], b.v[1]), T::mul(a.v[1], b.v[0]));
		}
		return r;
	}

	// ─── Length / Normalize ─────────────────────────────────────────────────

	template <int N, FieldWidth WIDTH>
	FORCE_INLINE typename VecLocal<N, WIDTH>::Val LengthSq(const VecLocal<N, WIDTH>& v)
	{
		if constexpr (N == 2) return Dot2<WIDTH>(v, v);
		else if constexpr (N == 3) return Dot3<WIDTH>(v, v);
		else return Dot4<WIDTH>(v, v);
	}

	template <int N, FieldWidth WIDTH>
	FORCE_INLINE typename VecLocal<N, WIDTH>::Val Length(const VecLocal<N, WIDTH>& v)
	{
		auto sq = LengthSq<N, WIDTH>(v);
		if constexpr (WIDTH == FieldWidth::Scalar) return std::sqrt(sq);
		else return _mm256_sqrt_ps(sq);
	}

	template <int N, FieldWidth WIDTH>
	FORCE_INLINE VecLocal<N, WIDTH> Normalized(const VecLocal<N, WIDTH>& v)
	{
		VecLocal<N, WIDTH> r;
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			float len = Length<N, WIDTH>(v);
			float inv = (len > 0.0f) ? 1.0f / len : 0.0f;
			for (int i = 0; i < N; ++i) r.v[i] = v.v[i] * inv;
		}
		else
		{
			using T    = SIMDTraits<float, WIDTH>;
			__m256 sq  = LengthSq<N, WIDTH>(v);
			__m256 inv = _mm256_rsqrt_ps(sq); // Fast approximate 1/sqrt
			for (int i = 0; i < N; ++i) r.v[i] = T::mul(v.v[i], inv);
		}
		return r;
	}
} // namespace VecMath

// ═════════════════════════════════════════════════════════════════════════
// Accessor structs — embed in components via reference members.
//
//   struct MyComponent {
//       FloatProxy<WIDTH> X, Y, Z;
//       Vec3Accessor<WIDTH> Position{X, Y, Z};
//   };
//
// 24 bytes on stack (Vec3) — 3 references, no data duplication.
// ═════════════════════════════════════════════════════════════════════════

template <FieldWidth WIDTH>
struct Vec2Accessor
{
	FloatProxy<WIDTH>& x;
	FloatProxy<WIDTH>& y;

	using VL  = VecMath::VecLocal<2, WIDTH>;
	using Val = typename VL::Val;

	// ── Load to local ──
	FORCE_INLINE VL Load() const { return VecMath::Load2<WIDTH>(x, y); }

	// ── Store from local ──
	FORCE_INLINE Vec2Accessor& operator=(const VL& v)
	{
		x = v.v[0];
		y = v.v[1];
		return *this;
	}

	// ── Compound assignment ──
	FORCE_INLINE Vec2Accessor& operator+=(const VL& v)
	{
		x += v.v[0];
		y += v.v[1];
		return *this;
	}

	FORCE_INLINE Vec2Accessor& operator-=(const VL& v)
	{
		x -= v.v[0];
		y -= v.v[1];
		return *this;
	}

	FORCE_INLINE Vec2Accessor& operator+=(const Vec2Accessor& o)
	{
		auto v = o.Load();
		x      += v.v[0];
		y      += v.v[1];
		return *this;
	}

	FORCE_INLINE Vec2Accessor& operator-=(const Vec2Accessor& o)
	{
		auto v = o.Load();
		x      -= v.v[0];
		y      -= v.v[1];
		return *this;
	}

	FORCE_INLINE Vec2Accessor& operator*=(float s)
	{
		x *= s;
		y *= s;
		return *this;
	}

	FORCE_INLINE Vec2Accessor& operator/=(float s)
	{
		x /= s;
		y /= s;
		return *this;
	}

	// ── Binary → VecLocal ──
	FORCE_INLINE VL operator+(const Vec2Accessor& o) const { return Load() + o.Load(); }
	FORCE_INLINE VL operator-(const Vec2Accessor& o) const { return Load() - o.Load(); }
	FORCE_INLINE VL operator+(const VL& v) const { return Load() + v; }
	FORCE_INLINE VL operator-(const VL& v) const { return Load() - v; }
	FORCE_INLINE VL operator*(float s) const { return Load() * s; }
	FORCE_INLINE VL operator/(float s) const { return Load() / s; }
	FORCE_INLINE VL operator-() const { return -Load(); }
	FORCE_INLINE friend VL operator*(float s, const Vec2Accessor& a) { return a.Load() * s; }

	// ── Queries ──
	FORCE_INLINE Val Dot(const Vec2Accessor& o) const { return VecMath::Dot2<WIDTH>(Load(), o.Load()); }
	FORCE_INLINE Val Dot(const VL& v) const { return VecMath::Dot2<WIDTH>(Load(), v); }
	FORCE_INLINE Val LengthSq() const { return VecMath::LengthSq<2, WIDTH>(Load()); }
	FORCE_INLINE Val Length() const { return VecMath::Length<2, WIDTH>(Load()); }

	// ── Mutation ──
	FORCE_INLINE void Normalize() { *this = VecMath::Normalized<2, WIDTH>(Load()); }
	FORCE_INLINE VL Normalized() const { return VecMath::Normalized<2, WIDTH>(Load()); }
	FORCE_INLINE void Set(float vx, float vy)
	{
		x = vx;
		y = vy;
	}
};

template <FieldWidth WIDTH>
struct Vec3Accessor
{
	FloatProxy<WIDTH>& x;
	FloatProxy<WIDTH>& y;
	FloatProxy<WIDTH>& z;

	using VL  = VecMath::VecLocal<3, WIDTH>;
	using Val = typename VL::Val;

	// ── Load to local ──
	FORCE_INLINE VL Load() const { return VecMath::Load3<WIDTH>(x, y, z); }

	// ── Store from local ──
	FORCE_INLINE Vec3Accessor& operator=(const VL& v)
	{
		x = v.v[0];
		y = v.v[1];
		z = v.v[2];
		return *this;
	}

	// ── Compound assignment ──
	FORCE_INLINE Vec3Accessor& operator+=(const VL& v)
	{
		x += v.v[0];
		y += v.v[1];
		z += v.v[2];
		return *this;
	}

	FORCE_INLINE Vec3Accessor& operator-=(const VL& v)
	{
		x -= v.v[0];
		y -= v.v[1];
		z -= v.v[2];
		return *this;
	}

	FORCE_INLINE Vec3Accessor& operator+=(const Vec3Accessor& o)
	{
		auto v = o.Load();
		x      += v.v[0];
		y      += v.v[1];
		z      += v.v[2];
		return *this;
	}

	FORCE_INLINE Vec3Accessor& operator-=(const Vec3Accessor& o)
	{
		auto v = o.Load();
		x      -= v.v[0];
		y      -= v.v[1];
		z      -= v.v[2];
		return *this;
	}

	FORCE_INLINE Vec3Accessor& operator*=(float s)
	{
		x *= s;
		y *= s;
		z *= s;
		return *this;
	}

	FORCE_INLINE Vec3Accessor& operator/=(float s)
	{
		x /= s;
		y /= s;
		z /= s;
		return *this;
	}

	// ── Binary → VecLocal ──
	FORCE_INLINE VL operator+(const Vec3Accessor& o) const { return Load() + o.Load(); }
	FORCE_INLINE VL operator-(const Vec3Accessor& o) const { return Load() - o.Load(); }
	FORCE_INLINE VL operator+(const VL& v) const { return Load() + v; }
	FORCE_INLINE VL operator-(const VL& v) const { return Load() - v; }
	FORCE_INLINE VL operator*(float s) const { return Load() * s; }
	FORCE_INLINE VL operator/(float s) const { return Load() / s; }
	FORCE_INLINE VL operator-() const { return -Load(); }
	FORCE_INLINE friend VL operator*(float s, const Vec3Accessor& a) { return a.Load() * s; }

	// ── Queries ──
	FORCE_INLINE Val Dot(const Vec3Accessor& o) const { return VecMath::Dot3<WIDTH>(Load(), o.Load()); }
	FORCE_INLINE Val Dot(const VL& v) const { return VecMath::Dot3<WIDTH>(Load(), v); }
	FORCE_INLINE Val LengthSq() const { return VecMath::LengthSq<3, WIDTH>(Load()); }
	FORCE_INLINE Val Length() const { return VecMath::Length<3, WIDTH>(Load()); }
	FORCE_INLINE VL Cross(const Vec3Accessor& o) const { return VecMath::Cross3<WIDTH>(Load(), o.Load()); }
	FORCE_INLINE VL Cross(const VL& v) const { return VecMath::Cross3<WIDTH>(Load(), v); }

	// ── Mutation ──
	FORCE_INLINE void Normalize() { *this = VecMath::Normalized<3, WIDTH>(Load()); }
	FORCE_INLINE VL Normalized() const { return VecMath::Normalized<3, WIDTH>(Load()); }
	FORCE_INLINE void Set(float vx, float vy, float vz)
	{
		x = vx;
		y = vy;
		z = vz;
	}
};

template <FieldWidth WIDTH>
struct Vec4Accessor
{
	FloatProxy<WIDTH>& x;
	FloatProxy<WIDTH>& y;
	FloatProxy<WIDTH>& z;
	FloatProxy<WIDTH>& w;

	using VL  = VecMath::VecLocal<4, WIDTH>;
	using Val = typename VL::Val;

	// ── Load to local ──
	FORCE_INLINE VL Load() const { return VecMath::Load4<WIDTH>(x, y, z, w); }

	// ── Store from local ──
	FORCE_INLINE Vec4Accessor& operator=(const VL& v)
	{
		x = v.v[0];
		y = v.v[1];
		z = v.v[2];
		w = v.v[3];
		return *this;
	}

	// ── Compound assignment ──
	FORCE_INLINE Vec4Accessor& operator+=(const VL& v)
	{
		x += v.v[0];
		y += v.v[1];
		z += v.v[2];
		w += v.v[3];
		return *this;
	}

	FORCE_INLINE Vec4Accessor& operator-=(const VL& v)
	{
		x -= v.v[0];
		y -= v.v[1];
		z -= v.v[2];
		w -= v.v[3];
		return *this;
	}

	FORCE_INLINE Vec4Accessor& operator+=(const Vec4Accessor& o)
	{
		auto v = o.Load();
		x      += v.v[0];
		y      += v.v[1];
		z      += v.v[2];
		w      += v.v[3];
		return *this;
	}

	FORCE_INLINE Vec4Accessor& operator-=(const Vec4Accessor& o)
	{
		auto v = o.Load();
		x      -= v.v[0];
		y      -= v.v[1];
		z      -= v.v[2];
		w      -= v.v[3];
		return *this;
	}

	FORCE_INLINE Vec4Accessor& operator*=(float s)
	{
		x *= s;
		y *= s;
		z *= s;
		w *= s;
		return *this;
	}

	FORCE_INLINE Vec4Accessor& operator/=(float s)
	{
		x /= s;
		y /= s;
		z /= s;
		w /= s;
		return *this;
	}

	// ── Binary → VecLocal ──
	FORCE_INLINE VL operator+(const Vec4Accessor& o) const { return Load() + o.Load(); }
	FORCE_INLINE VL operator-(const Vec4Accessor& o) const { return Load() - o.Load(); }
	FORCE_INLINE VL operator+(const VL& v) const { return Load() + v; }
	FORCE_INLINE VL operator-(const VL& v) const { return Load() - v; }
	FORCE_INLINE VL operator*(float s) const { return Load() * s; }
	FORCE_INLINE VL operator/(float s) const { return Load() / s; }
	FORCE_INLINE VL operator-() const { return -Load(); }
	FORCE_INLINE friend VL operator*(float s, const Vec4Accessor& a) { return a.Load() * s; }

	// ── Queries ──
	FORCE_INLINE Val Dot(const Vec4Accessor& o) const { return VecMath::Dot4<WIDTH>(Load(), o.Load()); }
	FORCE_INLINE Val Dot(const VL& v) const { return VecMath::Dot4<WIDTH>(Load(), v); }
	FORCE_INLINE Val LengthSq() const { return VecMath::LengthSq<4, WIDTH>(Load()); }
	FORCE_INLINE Val Length() const { return VecMath::Length<4, WIDTH>(Load()); }

	// ── Mutation ──
	FORCE_INLINE void Normalize() { *this = VecMath::Normalized<4, WIDTH>(Load()); }
	FORCE_INLINE VL Normalized() const { return VecMath::Normalized<4, WIDTH>(Load()); }
	FORCE_INLINE void Set(float vx, float vy, float vz, float vw)
	{
		x = vx;
		y = vy;
		z = vz;
		w = vw;
	}
};
