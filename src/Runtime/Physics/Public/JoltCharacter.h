#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

#include "SimFloat.h"

class JoltPhysics;

class JoltCharacter
{
public:
	~JoltCharacter();

	void Initialize(JoltPhysics* physics, JPH::RVec3 position,
		float capsuleRadius, float capsuleHalfHeight);

	void Shutdown();

	void Update(JPH::Vec3 desiredVelocity, JPH::Vec3 gravity, float dt,
		JPH::TempAllocator& allocator);

	JPH::RVec3 GetPosition() const;
	JPH::Quat GetRotation() const;
	JPH::Vec3 GetLinearVelocity() const;
	bool IsGrounded() const;

	void SetPosition(JPH::RVec3 position);

	void SyncToSlab(SimFloat* posX, SimFloat* posY, SimFloat* posZ,
		SimFloat* rotX, SimFloat* rotY, SimFloat* rotZ, SimFloat* rotW,
		uint32_t index);

	// Pass to JoltPhysics::BindConstructOnHit to wire up contact callbacks.
	JPH::BodyID GetInnerBodyID() const { return InnerBodyID; }

private:
	JPH::Ref<JPH::CharacterVirtual> Character;
	JoltPhysics* Physics = nullptr;
	JPH::BodyID InnerBodyID;
};
