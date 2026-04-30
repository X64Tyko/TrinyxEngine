#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include "RegistryTypes.h"

class JoltPhysics;

class JoltCharacter
{
public:
	~JoltCharacter();

	void Initialize(JoltPhysics* physics, EntityCacheHandle entityIndex, JPH::RVec3 position,
					float capsuleRadius, float capsuleHalfHeight);

	void Shutdown();

	// Called from Construct PrePhysics — feed it desired movement
	void Update(JPH::Vec3 desiredVelocity, JPH::Vec3 gravity, float dt,
				JPH::TempAllocator& allocator);

	// Read back results
	JPH::RVec3 GetPosition() const;
	JPH::Quat GetRotation() const;
	JPH::Vec3 GetLinearVelocity() const;
	bool IsGrounded() const;

	// Teleport character to a new position (e.g., server correction).
	void SetPosition(JPH::RVec3 position);

	// Write results back to slab fields
	void SyncToSlab(SimFloat* posX, SimFloat* posY, SimFloat* posZ,
					SimFloat* rotX, SimFloat* rotY, SimFloat* rotZ, SimFloat* rotW,
					uint32_t index);

private:
	JPH::Ref<JPH::CharacterVirtual> Character;
	JoltPhysics* Physics = nullptr;
};
