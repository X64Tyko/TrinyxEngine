#pragma once
#include "EInterpEntity.h"
#include "CMeshRef.h"
#include "CColor.h"
#include "CScale.h"
#include "CSkeletonRef.h"
#include "CAnimBase.h"
#include "CAnimLayer.h"

// -----------------------------------------------------------------------
// ESkeletalEntity — CRTP base for GPU-skinned skeletal entities.
//
// Holds the minimum slab data required for GPU skinning and rollback:
//   CMeshRef      (Volatile) — mesh slot; skin weights are stored in the same asset
//   CSkeletonRef  (Volatile) — skeleton slot for bone hierarchy and pose evaluation
//   CAnimBase     (Temporal) — base layer: 2-slot cross-fade or blendspace + state node
//   CAnimLayer    (Temporal) — overlay layers: up to 2 masked replace/additive slots
//
// All animation logic lives in an AnimConstruct (a Construct<T> companion object).
// The AnimConstruct runs:
//   PrePhysics  — applies root motion delta from the previous frame
//   PostPhysics — runs the state machine, writes new AnimBase/AnimLayer fields,
//                 handles loop wrapping, and fires notifies
//
// Wide PostPhysics: unconditionally advances all timestamps by dt via SIMD broadcast.
// Inactive slot timestamps advance harmlessly — AnimConstruct resets them on slot activation.
// -----------------------------------------------------------------------

template <template <FieldWidth> class Derived, FieldWidth WIDTH = FieldWidth::Scalar>
class ESkeletalEntity : public EInterpEntity<Derived, WIDTH>
{
	TNX_REGISTER_SUPER_SCHEMA(ESkeletalEntity, EInterpEntity, Mesh, Color, Scale, SkeletonRef, AnimBase, AnimLayer)

public:
	CMeshRef<WIDTH> Mesh;
	CColor<WIDTH> Color;
	CScale<WIDTH> Scale;
	CSkeletonRef<WIDTH> SkeletonRef;
	CAnimBase<WIDTH> AnimBase;
	CAnimLayer<WIDTH> AnimLayer;

	void Initialize()
	{
		EInterpEntity<Derived, WIDTH>::Initialize();
		Color.R      = SimFloat(1.f);
		Color.G      = SimFloat(1.f);
		Color.B      = SimFloat(1.f);
		Color.A      = SimFloat(1.f);
		Scale.ScaleX = SimFloat(1.f);
		Scale.ScaleY = SimFloat(1.f);
		Scale.ScaleZ = SimFloat(1.f);
		AnimBase.Clear();
		AnimLayer.Clear();
	}

	void PostPhysics(SimFloat dt)
	{
		AnimBase.BaseTimestamp += dt;
		AnimBase.FadeTimestamp += dt;
		AnimLayer.LayerTimestamp0 += dt;
		AnimLayer.LayerTimestamp1 += dt;
		EInterpEntity<Derived, WIDTH>::PostPhysics(dt);
	}
};
