#pragma once
#include "SimFloat.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

// Quaternion dot product.
template <typename TRot>
FORCE_INLINE TRot QDot(TRot ax, TRot ay, TRot az, TRot aw,
	TRot bx, TRot by, TRot bz, TRot bw)
{
	return ax * bx + ay * by + az * bz + aw * bw;
}

// Normalized quaternion lerp. Flips b for shortest-arc then re-normalizes.
// TRot is typically SimUnit; t is the blend weight as SimFloat.
template <typename TRot>
FORCE_INLINE void QNLerp(TRot ax, TRot ay, TRot az, TRot aw,
	TRot bx, TRot by, TRot bz, TRot bw,
	SimFloat t,
	TRot& ox, TRot& oy, TRot& oz, TRot& ow)
{
	if (QDot(ax, ay, az, aw, bx, by, bz, bw) < TRot(0))
	{
		bx = -bx;
		by = -by;
		bz = -bz;
		bw = -bw;
	}
	SimFloat it = SimFloat(1) - t;
	ox          = it * ax + t * bx;
	oy          = it * ay + t * by;
	oz          = it * az + t * bz;
	ow          = it * aw + t * bw;
	TRot len    = Sqrt(ox * ox + oy * oy + oz * oz + ow * ow);
	if (len > TRot(0))
	{
		ox /= len;
		oy /= len;
		oz /= len;
		ow /= len;
	}
}

#ifdef TNX_DETERMINISM
// 128-bit signed multiply, right-shifted by `shift` bits, returned as int64.
// Used by RotateVectorFixed to avoid int64 overflow in the second-level products
// now that FixedUnit::Scale = 1<<28.
FORCE_INLINE int64_t Mul128Shift(int64_t a, int64_t b, int shift)
{
#ifdef _MSC_VER
	int64_t hi;
	const uint64_t lo = static_cast<uint64_t>(_mul128(a, b, &hi));
	const int rhi     = shift - 64;
	const int rlo     = shift;
	if (rhi >= 0)
		return hi >> rhi;
	return static_cast<int64_t>((static_cast<uint64_t>(hi) << -rhi) | (lo >> rlo));
#else
	return static_cast<int64_t>((__int128)a * b >> shift);
#endif
}

// Rotate a Fixed32 metric vector by a unit quaternion (raw FixedUnit components).
// Accumulates cross products in int64, uses 128-bit for the second-level products
// (FixedUnit::Scale^2 = 1<<56 exceeds int64 for vectors beyond ~26m), then divides
// by 1<<(2*ScaleLog2) = 1<<56 once. Safe for bone-space vectors (<<10m).
inline void RotateVectorFixed(
	int32_t qx, int32_t qy, int32_t qz, int32_t qw,
	int32_t vx, int32_t vy, int32_t vz,
	int32_t& ox, int32_t& oy, int32_t& oz)
{
	constexpr int kShift = 2 * FixedUnit::ScaleLog2; // 56
	const int64_t cx     = 2LL * ((int64_t)qy * vz - (int64_t)qz * vy);
	const int64_t cy     = 2LL * ((int64_t)qz * vx - (int64_t)qx * vz);
	const int64_t cz     = 2LL * ((int64_t)qx * vy - (int64_t)qy * vx);
	ox                   = vx + static_cast<int32_t>(Mul128Shift(qw, cx, kShift) + Mul128Shift(qy, cz, kShift) - Mul128Shift(qz, cy, kShift));
	oy                   = vy + static_cast<int32_t>(Mul128Shift(qw, cy, kShift) + Mul128Shift(qz, cx, kShift) - Mul128Shift(qx, cz, kShift));
	oz                   = vz + static_cast<int32_t>(Mul128Shift(qw, cz, kShift) + Mul128Shift(qx, cy, kShift) - Mul128Shift(qy, cx, kShift));
}
#endif
