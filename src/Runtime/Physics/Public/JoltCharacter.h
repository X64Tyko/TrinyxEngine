#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

class JoltCharacter
{
public:
	void Initialize(JPH::PhysicsSystem* system, JPH::RVec3 position,
					float capsuleRadius, float capsuleHalfHeight);

	// Called from Construct PrePhysics — feed it desired movement
	void Update(JPH::Vec3 desiredVelocity, JPH::Vec3 gravity, float dt,
				JPH::TempAllocator& allocator);

	// Read back results
	JPH::RVec3 GetPosition() const;
	JPH::Quat GetRotation() const;
	JPH::Vec3 GetLinearVelocity() const;
	bool IsGrounded() const;

	// Write results back to slab fields
	void SyncToSlab(float* posX, float* posY, float* posZ,
					float* rotX, float* rotY, float* rotZ, float* rotW,
					uint32_t index);

private:
	JPH::Ref<JPH::CharacterVirtual> Character;
	JPH::PhysicsSystem* PhysSystem = nullptr;
};
