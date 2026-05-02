#pragma once

#include "FieldProxy.h"
#include "FastTrig.h"
#include <cmath>
#include "ComponentView.h"

// QuatMath — quaternion operations on FieldProxy<SimFloat, WIDTH>.
//
// Solves the cross-dependency problem: all 4 components are loaded into
// locals before any writes, so the result is correct at every FieldWidth
// (Scalar, Wide, WideMask).
//
// Entity authors should not call these directly — use the convenience
// methods on TransRot / Rotation components instead (RotateX, RotateY, etc.).
namespace QuatMath
{
	// Intermediate storage: float in Scalar, __m256 in Wide/WideMask.
	template <FieldWidth WIDTH>
	struct QuatLocal
	{
		using Val = std::conditional_t<WIDTH == FieldWidth::Scalar, SimFloat, __m256>;
		Val x, y, z, w;
	};

	// Load all 4 quaternion components atomically (no writes between reads).
	template <FieldWidth WIDTH>
	FORCE_INLINE QuatLocal<WIDTH> Load(
		const FloatProxy<WIDTH>& qx, const FloatProxy<WIDTH>& qy,
		const FloatProxy<WIDTH>& qz, const FloatProxy<WIDTH>& qw)
	{
		QuatLocal<WIDTH> q;
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			q.x = qx.WriteArray[qx.index];
			q.y = qy.WriteArray[qy.index];
			q.z = qz.WriteArray[qz.index];
			q.w = qw.WriteArray[qw.index];
		}
		else
		{
			using T = SIMDTraits<SimFloat, WIDTH>;
			q.x     = T::load(&qx.WriteArray[qx.index]);
			q.y     = T::load(&qy.WriteArray[qy.index]);
			q.z     = T::load(&qz.WriteArray[qz.index]);
			q.w     = T::load(&qw.WriteArray[qw.index]);
		}
		return q;
	}

	// Write all 4 components back via operator= (handles mask + dirty bits).
	template <FieldWidth WIDTH>
FORCE_INLINE void Store(
		FloatProxy<WIDTH>& qx, FloatProxy<WIDTH>& qy,
		FloatProxy<WIDTH>& qz, FloatProxy<WIDTH>& qw,
		const QuatLocal<WIDTH>& q)
	{
		qx = q.x;
		qy = q.y;
		qz = q.z;
		qw = q.w;
	}

	// Hamilton product: result = a * b.
	// b is a uniform scalar quaternion (same delta rotation for all entities in the lane).
	template <FieldWidth WIDTH>
	FORCE_INLINE QuatLocal<WIDTH> Multiply(const QuatLocal<WIDTH>& a,
										   SimFloat bx, SimFloat by, SimFloat bz, SimFloat bw)
	{
		QuatLocal<WIDTH> r;
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			r.x = a.x * bw + a.w * bx + a.y * bz - a.z * by;
			r.y = a.y * bw + a.w * by + a.z * bx - a.x * bz;
			r.z = a.z * bw + a.w * bz + a.x * by - a.y * bx;
			r.w = a.w * bw - a.x * bx - a.y * by - a.z * bz;
		}
		else
		{
			using T         = SIMDTraits<SimFloat, WIDTH>;
			const __m256 Bx = T::set1(bx), By = T::set1(by);
			const __m256 Bz = T::set1(bz), Bw = T::set1(bw);

			// r.x = a.x*Bw + a.w*Bx + a.y*Bz - a.z*By
			r.x = T::add(T::add(T::mul(a.x, Bw), T::mul(a.w, Bx)),
						 T::sub(T::mul(a.y, Bz), T::mul(a.z, By)));
			// r.y = a.y*Bw + a.w*By + a.z*Bx - a.x*Bz
			r.y = T::add(T::add(T::mul(a.y, Bw), T::mul(a.w, By)),
						 T::sub(T::mul(a.z, Bx), T::mul(a.x, Bz)));
			// r.z = a.z*Bw + a.w*Bz + a.x*By - a.y*Bx
			r.z = T::add(T::add(T::mul(a.z, Bw), T::mul(a.w, Bz)),
						 T::sub(T::mul(a.x, By), T::mul(a.y, Bx)));
			// r.w = a.w*Bw - a.x*Bx - a.y*By - a.z*Bz
			r.w = T::sub(T::sub(T::sub(T::mul(a.w, Bw), T::mul(a.x, Bx)),
								T::mul(a.y, By)), T::mul(a.z, Bz));
		}
		return r;
	}

	// Rotate by axis-angle. Axis must be unit length.
	// Computes delta quaternion from axis-angle, then Hamilton-multiplies.
	template <FieldWidth WIDTH>
FORCE_INLINE void RotateAxisAngle(
		FloatProxy<WIDTH>& qx, FloatProxy<WIDTH>& qy,
		FloatProxy<WIDTH>& qz, FloatProxy<WIDTH>& qw,
		SimFloat ax, SimFloat ay, SimFloat az, SimFloat angle)
	{
		const SimFloat half = angle * SimFloat(0.5f);
		const SimFloat s    = FastSin(half), c = FastCos(half);
		auto q           = Load<WIDTH>(qx, qy, qz, qw);
		auto r           = Multiply<WIDTH>(q, ax * s, ay * s, az * s, c);
		Store<WIDTH>(qx, qy, qz, qw, r);
	}

	// Convenience: rotate around world X axis.
	template <FieldWidth WIDTH>
FORCE_INLINE void RotateX(FloatProxy<WIDTH>& qx, FloatProxy<WIDTH>& qy,
						  FloatProxy<WIDTH>& qz, FloatProxy<WIDTH>& qw, SimFloat angle)
	{
		RotateAxisAngle<WIDTH>(qx, qy, qz, qw, 1.0f, 0.0f, 0.0f, angle);
	}

	// Convenience: rotate around world Y axis.
	template <FieldWidth WIDTH>
FORCE_INLINE void RotateY(FloatProxy<WIDTH>& qx, FloatProxy<WIDTH>& qy,
						  FloatProxy<WIDTH>& qz, FloatProxy<WIDTH>& qw, SimFloat angle)
	{
		RotateAxisAngle<WIDTH>(qx, qy, qz, qw, 0.0f, 1.0f, 0.0f, angle);
	}

	// Convenience: rotate around world Z axis.
	template <FieldWidth WIDTH>
FORCE_INLINE void RotateZ(FloatProxy<WIDTH>& qx, FloatProxy<WIDTH>& qy,
						  FloatProxy<WIDTH>& qz, FloatProxy<WIDTH>& qw, SimFloat angle)
	{
		RotateAxisAngle<WIDTH>(qx, qy, qz, qw, 0.0f, 0.0f, 1.0f, angle);
	}

	// Set to identity quaternion (0, 0, 0, 1).
	template <FieldWidth WIDTH>
FORCE_INLINE void SetIdentity(FloatProxy<WIDTH>& qx, FloatProxy<WIDTH>& qy,
							  FloatProxy<WIDTH>& qz, FloatProxy<WIDTH>& qw)
	{
		qx = SimFloat(0.0f);
		qy = SimFloat(0.0f);
		qz = SimFloat(0.0f);
		qw = SimFloat(1.0f);
	}
} // namespace QuatMath

// ═════════════════════════════════════════════════════════════════════════
// QuatAccessor — embed in components via reference members.
//
//   struct TransRot {
//       FloatProxy<WIDTH> RotQx, RotQy, RotQz, RotQw;
//       QuatAccessor<WIDTH> Rotation{RotQx, RotQy, RotQz, RotQw};
//   };
//
// 32 bytes on stack — 4 references, no data duplication.
// ═════════════════════════════════════════════════════════════════════════

template <FieldWidth WIDTH>
struct QuatAccessor
{
	FloatProxy<WIDTH>& qx;
	FloatProxy<WIDTH>& qy;
	FloatProxy<WIDTH>& qz;
	FloatProxy<WIDTH>& qw;

	using QL = QuatMath::QuatLocal<WIDTH>;

	// ── Load / Store ──
	FORCE_INLINE QL Load() const { return QuatMath::Load<WIDTH>(qx, qy, qz, qw); }
	FORCE_INLINE void Store(const QL& q) { QuatMath::Store<WIDTH>(qx, qy, qz, qw, q); }

	// ── Rotations ──
	FORCE_INLINE void RotateX(SimFloat angle) { QuatMath::RotateX<WIDTH>(qx, qy, qz, qw, angle); }
	FORCE_INLINE void RotateY(SimFloat angle) { QuatMath::RotateY<WIDTH>(qx, qy, qz, qw, angle); }
	FORCE_INLINE void RotateZ(SimFloat angle) { QuatMath::RotateZ<WIDTH>(qx, qy, qz, qw, angle); }

	FORCE_INLINE void RotateAxisAngle(SimFloat ax, SimFloat ay, SimFloat az, SimFloat angle)
	{
		QuatMath::RotateAxisAngle<WIDTH>(qx, qy, qz, qw, ax, ay, az, angle);
	}

	// ── Identity ──
	FORCE_INLINE void SetIdentity() { QuatMath::SetIdentity<WIDTH>(qx, qy, qz, qw); }

	// ── Multiply by uniform delta quaternion ──
	FORCE_INLINE void Multiply(SimFloat bx, SimFloat by, SimFloat bz, SimFloat bw)
	{
		auto q = Load();
		auto r = QuatMath::Multiply<WIDTH>(q, bx, by, bz, bw);
		Store(r);
	}
};

// Build a camera orientation quaternion from FPS-style yaw (world Y) and pitch (local X).
// yaw=0, pitch=0 → facing -Z with up=+Y.  Result is a unit quaternion.
inline Quat QuatFromYawPitch(SimFloat yaw, SimFloat pitch)
{
	const SimFloat hy = yaw   * SimFloat(0.5f);
	const SimFloat hp = pitch * SimFloat(0.5f);
	const Quat qY{SimFloat(0), FastSin(hy), SimFloat(0), FastCos(hy)};
	const Quat qP{FastSin(hp), SimFloat(0), SimFloat(0), FastCos(hp)};
	return qY * qP;
}
