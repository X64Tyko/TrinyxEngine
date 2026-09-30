#pragma once

#include "CameraManager.h"
#include "Construct.h"
#include "ConstructView.h"
#include "JoltCharacter.h"

#include "EPlayer.h"

// Camera layer used by PlayerConstruct — writes eye/orbit position to WorldCameraState.
struct PlayerCameraLayer : CameraLayer, CameraStateMix<PlayerCameraLayer>
{
	SimFloat PosX = SimFloat(0.f), PosY = SimFloat(0.f), PosZ = SimFloat(0.f);
	SimFloat Yaw = SimFloat(0.f), Pitch = SimFloat(0.f);
	SimFloat FOV = SimFloat(60.f);

	void ApplyState(WorldCameraState& state);
};

// PlayerConstruct — Player capsule with physics character controller and dual camera layers.
class PlayerConstruct : public Construct<PlayerConstruct>
{
	TNX_REGISTER_CONSTRUCT(PlayerConstruct)

public:
	TNX_CONSTRUCT_WORLD

	ConstructView<EPlayer> Body;
	JoltCharacter CharacterController;
	Vector3 PhysPos;

	~PlayerConstruct();

	void InitializeViews();
	void InitializeForReplication(WorldBase* world, EntityHandle* viewHandles, uint8_t viewCount);

	void PrePhysics(SimFloat dt);
	void PhysicsStep(SimFloat dt);
	void PostPhysics(SimFloat dt);

	void OnHit(PhysicsOnHitData data);

	SimFloat SpawnPosX = SimFloat(0.0f);
	SimFloat SpawnPosY = SimFloat(5.0f);
	SimFloat SpawnPosZ = SimFloat(0.0f);

	uint8_t GetOwnerID() const;

private:
	PlayerCameraLayer FPLayer; // first-person
	PlayerCameraLayer TPLayer; // third-person, default active

	bool bIsClientSide = false;
	EntityHandle ReplicationEntityHandle{};

	// Local Owner's view (presentation + the source of the view angles written into sim input).
	// Not simulation state: gameplay reads facing from sim input / CControlRotation.
	SimFloat Yaw         = SimFloat(0.0f);
	SimFloat Pitch       = SimFloat(0.0f);
	SimFloat DesiredVelX = SimFloat(0.0f);
	SimFloat DesiredVelZ = SimFloat(0.0f);
	bool bToggleHeld     = false;

	static constexpr SimFloat MoveSpeed = SimFloat(8.0f);
	static constexpr SimFloat EyeHeight = SimFloat(1.5f);
};
