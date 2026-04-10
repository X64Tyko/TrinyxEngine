#include "JoltCharacter.h"
#include "JoltLayers.h"
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>

void JoltCharacter::Initialize(JPH::PhysicsSystem* system, JPH::RVec3 position,
							   float capsuleRadius, float capsuleHalfHeight)
{
	PhysSystem = system;

	JPH::CharacterVirtualSettings settings;
	settings.mShape                     = new JPH::CapsuleShape(capsuleHalfHeight, capsuleRadius);
	settings.mMaxSlopeAngle             = JPH::DegreesToRadians(45.0f);
	settings.mMaxStrength               = 100.0f;
	settings.mCharacterPadding          = 0.02f;
	settings.mPenetrationRecoverySpeed  = 1.0f;
	settings.mPredictiveContactDistance = 0.1f;

	Character = new JPH::CharacterVirtual(
		&settings, position, JPH::Quat::sIdentity(), 0, PhysSystem);
}

void JoltCharacter::Update(JPH::Vec3 desiredVelocity, JPH::Vec3 gravity, float dt,
						   JPH::TempAllocator& allocator)
{
	JPH::Vec3 current_vertical_velocity = Character->GetLinearVelocity().Dot(Character->GetUp()) * Character->GetUp();
	JPH::Vec3 ground_velocity           = Character->GetGroundVelocity();
	JPH::Vec3 new_velocity;

	if (Character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround
		&& (current_vertical_velocity.GetY() - ground_velocity.GetY()) < 0.1f)
	{
		// On ground: adopt ground velocity (kills gravity accumulation)
		new_velocity = ground_velocity;
	}
	else
	{
		// In air: keep current vertical velocity (gravity keeps accumulating)
		new_velocity = current_vertical_velocity;
	}

	// Apply gravity (accumulate every frame)
	new_velocity += gravity * dt;

	// Add horizontal player input
	new_velocity += desiredVelocity;
	Character->SetLinearVelocity(new_velocity);

	// Extended update handles grounding, stepping, slope sliding
	JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
	updateSettings.mStickToFloorStepDown = JPH::Vec3(0, -0.5f, 0);
	updateSettings.mWalkStairsStepUp     = JPH::Vec3(0, 0.4f, 0);

	Character->ExtendedUpdate(
		dt,
		gravity,
		updateSettings,
		PhysSystem->GetDefaultBroadPhaseLayerFilter(JoltLayers::Dynamic),
		PhysSystem->GetDefaultLayerFilter(JoltLayers::Dynamic),
		{}, // body filter
		{}, // shape filter
		allocator);
}

JPH::RVec3 JoltCharacter::GetPosition() const { return Character->GetPosition(); }
JPH::Quat JoltCharacter::GetRotation() const { return Character->GetRotation(); }
JPH::Vec3 JoltCharacter::GetLinearVelocity() const { return Character->GetLinearVelocity(); }

bool JoltCharacter::IsGrounded() const
{
	return Character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
}

void JoltCharacter::SyncToSlab(float* posX, float* posY, float* posZ,
							   float* rotX, float* rotY, float* rotZ, float* rotW,
							   uint32_t index)
{
	JPH::RVec3 pos = GetPosition();
	JPH::Quat rot  = GetRotation();
	posX[index]    = pos.GetX();
	posY[index]    = pos.GetY();
	posZ[index]    = pos.GetZ();
	rotX[index]    = rot.GetX();
	rotY[index]    = rot.GetY();
	rotZ[index]    = rot.GetZ();
	rotW[index]    = rot.GetW();
}
