#pragma once

#include "ComponentView.h"
#include "SchemaReflector.h"
#include "VecMath.h"
#include "QuatMath.h"

// TransRot Component — Position + Quaternion rotation.
// The default "physics transform" for most entities. Jolt outputs exactly
// these 7 fields (float3 position + float4 quaternion).
// Scale is a separate Volatile component — see Scale.h.
//
// Lighter alternatives: Translation (pos only), Rotation (quat only).
//
// Vector/quaternion access via nested accessors:
//   transform.Position += velocity.Vel * fdt;
//   transform.Rotation.RotateY(angle);
//   transform.Rotation.SetIdentity();
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct CTransform : ComponentView<CTransform, WIDTH>
{
	TNX_TEMPORAL_FIELDS(CTransform, Physics, PosX, PosY, PosZ, RotQx, RotQy, RotQz, RotQw)

	FloatProxy<WIDTH> PosX;
	FloatProxy<WIDTH> PosY;
	FloatProxy<WIDTH> PosZ;

	FloatProxy<WIDTH> RotQx; // Quaternion (x, y, z, w) — Jolt-native, GPU-normalized
	FloatProxy<WIDTH> RotQy;
	FloatProxy<WIDTH> RotQz;
	FloatProxy<WIDTH> RotQw;

	// Nested accessors — hide FieldProxy plumbing from entity authors.
	// References point to sibling fields; no data duplication.
	Vec3Accessor<WIDTH> Position{PosX, PosY, PosZ};
	QuatAccessor<WIDTH> Rotation{RotQx, RotQy, RotQz, RotQw};

	static uint8_t GetTemporalIndex() { return 1; }
};

TNX_REGISTER_COMPONENT(CTransform)
