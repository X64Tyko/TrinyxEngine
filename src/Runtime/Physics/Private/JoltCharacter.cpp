#include "JoltCharacter.h"
#include "JoltLayers.h"
#include "JoltPhysics.h"
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>

JoltCharacter::~JoltCharacter()
{
	Shutdown();
}

void JoltCharacter::Initialize(JoltPhysics* physics, EntityCacheHandle entityIndex, JPH::RVec3 position,
							   float capsuleRadius, float capsuleHalfHeight)
{
	Physics = physics;

	JPH::CharacterVirtualSettings settings;
	settings.mShape                     = new JPH::CapsuleShape(capsuleHalfHeight, capsuleRadius);
	settings.mMaxSlopeAngle             = JPH::DegreesToRadians(45.0f);
	settings.mMaxStrength               = 100.0f;
	settings.mCharacterPadding          = 0.02f;
	settings.mPenetrationRecoverySpeed  = 1.0f;
	settings.mPredictiveContactDistance = 0.1f;
	settings.mInnerBodyShape            = settings.mShape;
	settings.mInnerBodyLayer            = JoltLayers::Dynamic;

	Character = new JPH::CharacterVirtual(
		&settings, position, JPH::Quat::sIdentity(), 0, Physics->GetPhysicsSystem());

	if (!Character->GetInnerBodyID().IsInvalid())
	{
		Physics->RegisterBody(Character->GetInnerBodyID(), entityIndex);
	}
}

void JoltCharacter::Shutdown()
{
	if (Character && Physics && Physics->GetIsActive())
	{
		// Unregister our owner mapping before the body goes away.
		if (!Character->GetInnerBodyID().IsInvalid())
		{
			Physics->UnregisterBody(Character->GetInnerBodyID());
		}
		
		Character = nullptr;
	}
	Physics = nullptr;
}

void JoltCharacter::Update(JPH::Vec3 desiredVelocity, JPH::Vec3 gravity, float dt,
						   JPH::TempAllocator& allocator)
{
	if (!Character) return;

	JPH::Vec3 current_vertical_velocity = Character->GetLinearVelocity().Dot(Character->GetUp()) * Character->GetUp();
	JPH::Vec3 ground_velocity           = Character->GetGroundVelocity();
	JPH::Vec3 new_velocity;

	if (Character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround
		&& (current_vertical_velocity.GetY() - ground_velocity.GetY()) < 0.1f)
	{
		// On ground: adopt ground velocity. Do NOT add gravity — the floor contact
		// constraint handles it. Adding gravity here pushes the character into the
		// floor every step and causes Jolt to resolve it back up, producing Y jitter.
		new_velocity = ground_velocity;
	}
	else
	{
		// In air: keep current vertical velocity and accumulate gravity.
		new_velocity  = current_vertical_velocity;
		new_velocity += gravity * dt;
	}

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
		Physics->GetPhysicsSystem()->GetDefaultBroadPhaseLayerFilter(JoltLayers::Dynamic),
		Physics->GetPhysicsSystem()->GetDefaultLayerFilter(JoltLayers::Dynamic),
		{}, // body filter
		{}, // shape filter
		allocator);
}

JPH::RVec3 JoltCharacter::GetPosition() const { return Character->GetPosition(); }
JPH::Quat JoltCharacter::GetRotation() const { return Character->GetRotation(); }
JPH::Vec3 JoltCharacter::GetLinearVelocity() const { return Character->GetLinearVelocity(); }
void JoltCharacter::SetPosition(JPH::RVec3 position) { if (Character) Character->SetPosition(position); }

bool JoltCharacter::IsGrounded() const
{
	return Character ? Character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround : false;
}

void JoltCharacter::SyncToSlab(SimFloat* posX, SimFloat* posY, SimFloat* posZ,
							   SimFloat* rotX, SimFloat* rotY, SimFloat* rotZ, SimFloat* rotW,
							   uint32_t index)
{
	if (!Character) return;
	JPH::RVec3 pos = GetPosition();
	JPH::Quat rot  = GetRotation();
	posX[index]    = SimFloat(pos.GetX());
	posY[index]    = SimFloat(pos.GetY());
	posZ[index]    = SimFloat(pos.GetZ());
	rotX[index]    = SimFloat(rot.GetX());
	rotY[index]    = SimFloat(rot.GetY());
	rotZ[index]    = SimFloat(rot.GetZ());
	rotW[index]    = SimFloat(rot.GetW());
}
