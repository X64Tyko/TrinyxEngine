#pragma once
#include "ESkeletalEntity.h"
#include "SchemaReflector.h"

// -----------------------------------------------------------------------
// EPlayerCharacter — skeletal entity for a player character.
//
// Carries no animation logic itself. An owning AnimConstruct (the equivalent
// of an AnimBP) drives CAnimBase and CAnimLayer each PostPhysics:
//   - Locomotion blendspace coords from physics-resolved velocity
//   - Upper body weapon layer transitions
//   - State machine node position and cross-fade alpha
//
// Socket queries (weapon attach point, foot IK) go through the AnimConstruct's
// CPU bone cache, not through this entity.
// -----------------------------------------------------------------------

template <FieldWidth WIDTH = FieldWidth::Scalar>
class EPlayerCharacter : public ESkeletalEntity<EPlayerCharacter, WIDTH>
{
	TNX_REGISTER_SCHEMA(EPlayerCharacter, ESkeletalEntity)

	void Initialize()
	{
		ESkeletalEntity<EPlayerCharacter, WIDTH>::Initialize();
	}

	void PostPhysics(SimFloat dt)
	{
		ESkeletalEntity<EPlayerCharacter, WIDTH>::PostPhysics(dt);
	}
};
