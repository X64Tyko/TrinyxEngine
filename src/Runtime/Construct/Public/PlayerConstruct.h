#pragma once

#include "CameraConstruct.h"
#include "Construct.h"
#include "ConstructView.h"
#include "EngineConfig.h"
#include "Input.h"
#include "JoltCharacter.h"
#include "JoltPhysics.h"
#include "Owned.h"
#include "Soul.h"

#include "EPlayer.h"

#include <cmath>

// ---------------------------------------------------------------------------
// PlayerConstruct — Standard engine player with physics capsule and dual cameras.
//
// Owns a ConstructView<EPlayer> (capsule mesh, physics body) and two
// CameraConstructs (first-person + third-person). V key toggles the active
// camera. WASD drives kinematic velocity (set in PrePhysics, consumed by Jolt).
// Mouse look and camera positioning happen in ScalarUpdate (after physics).
//
// Ownership: GetOwnerSoul() returns the Soul* set during replication
// (or null in standalone/server). GetOwnerID() delegates to that Soul.
// ---------------------------------------------------------------------------
class PlayerConstruct : public Construct<PlayerConstruct>
{
public:
	TNX_CONSTRUCT_WORLD

	ConstructView<EPlayer> Body;
	Owned<CameraConstruct> FirstPersonCam;
	Owned<CameraConstruct> ThirdPersonCam;

	JoltCharacter CharacterController;

	void InitializeViews()
	{
		if (bIsClientSide)
		{
			// Client-side: attach to the existing ECS entity delivered by ConstructSpawn,
			// then read its authoritative position to seed the JoltCharacter correctly.
			Body.Attach(this, ReplicationEntityHandle);
			SpawnPosX = Body.Transform.PosX.Value();
			SpawnPosY = Body.Transform.PosY.Value();
			SpawnPosZ = Body.Transform.PosZ.Value();
		}
		else
		{
			Body.Initialize(this);

			auto& tr = Body.Transform;
			tr.PosX  = SpawnPosX;
			tr.PosY  = SpawnPosY;
			tr.PosZ  = SpawnPosZ;

			tr.Rotation.SetIdentity();

			auto& sc  = Body.Scale;
			sc.ScaleX = 1.0f;
			sc.ScaleY = 1.0f;
			sc.ScaleZ = 1.0f;

			auto& col = Body.Color;
			col.R     = 0.2f;
			col.G     = 0.8f;
			col.B     = 0.2f;
			col.A     = 1.0f;

			auto& mesh  = Body.Mesh;
			mesh.MeshID = 1u; // Capsule (slot 0=Cube, slot 1=Capsule)

			Body.SetFlags(TemporalFlagBits::Active | TemporalFlagBits::Replicated);
		}

		auto* phys = GetWorld()->GetPhysics();
		CharacterController.Initialize(
			phys->GetPhysicsSystem(),
			JPH::RVec3(SpawnPosX, SpawnPosY, SpawnPosZ),
			0.3f,  // capsule radius
			0.7f); // capsule half height

		// Initialize cameras (both paths)
		FirstPersonCam->Initialize(GetWorld());
		ThirdPersonCam->Initialize(GetWorld());

		// Default to third-person (better for visualizing corrections)
		ActiveCam = ThirdPersonCam.Get();
		SetActiveCameraIfOwned(ActiveCam);
	}

	/// Replication entry point — called by ConstructRegistry::CreateForReplication.
	/// Attaches to existing ECS entities instead of creating new ones.
	void InitializeForReplication(World* world, EntityHandle* viewHandles, uint8_t viewCount)
	{
		bIsClientSide = true;
		if (viewCount > 0) ReplicationEntityHandle = viewHandles[0];
		Initialize(world);
	}

	void PhysicsStep(SimFloat dt)
	{
		CharacterController.Update(
			JPH::Vec3(DesiredVelX, 0, DesiredVelZ),
			JPH::Vec3(0, -9.81f, 0),
			static_cast<float>(dt),
			*GetWorld()->GetPhysics()->GetTempAllocator());

		JPH::RVec3 pos      = CharacterController.GetPosition();
		Body.Transform.PosX = pos.GetX();
		Body.Transform.PosY = pos.GetY();
		Body.Transform.PosZ = pos.GetZ();

		DesiredVelX = 0.0f;
		DesiredVelZ = 0.0f;
	}

	void PrePhysics(SimFloat /*dt*/)
	{
		InputBuffer* simInput = GetWorld()->GetInputForPlayer(GetOwnerID());

		float sinYaw = std::sin(Yaw);
		float cosYaw = std::cos(Yaw);

		float forwardX = sinYaw, forwardZ = -cosYaw;
		float rightX   = cosYaw, rightZ   = sinYaw;

		float moveX = 0.0f, moveZ = 0.0f;

		if (simInput->IsActionDown(Action::MoveForward))  { moveX += forwardX; moveZ += forwardZ; }
		if (simInput->IsActionDown(Action::MoveBackward)) { moveX -= forwardX; moveZ -= forwardZ; }
		if (simInput->IsActionDown(Action::MoveRight))    { moveX += rightX;   moveZ += rightZ;   }
		if (simInput->IsActionDown(Action::MoveLeft))     { moveX -= rightX;   moveZ -= rightZ;   }

		float len = std::sqrt(moveX * moveX + moveZ * moveZ);
		if (len > 0.001f)
		{
			DesiredVelX += moveX / len * MoveSpeed;
			DesiredVelZ += moveZ / len * MoveSpeed;
		}
	}

	void ScalarUpdate(SimFloat /*dt*/)
	{
		InputBuffer* vizInput = GetWorld()->GetVizInputForPlayer(GetOwnerID());

		bool toggleDown = vizInput->IsActionDown(Action::ToggleCamera);
		if (toggleDown && !bToggleHeld)
		{
			if (ActiveCam == FirstPersonCam.Get()) ActiveCam = ThirdPersonCam.Get();
			else ActiveCam                                   = FirstPersonCam.Get();

			SetActiveCameraIfOwned(ActiveCam);
		}
		bToggleHeld = toggleDown;

		constexpr float MouseSens = 0.002f;
		constexpr float MaxPitch  = 1.5533f; // ~89 degrees

		Yaw   += vizInput->GetMouseDX() * MouseSens;
		Pitch -= vizInput->GetMouseDY() * MouseSens;
		if (Pitch > MaxPitch) Pitch = MaxPitch;
		if (Pitch < -MaxPitch) Pitch = -MaxPitch;

		auto& tr    = Body.Transform;
		SimFloat px = tr.PosX.Value();
		SimFloat py = tr.PosY.Value();
		SimFloat pz = tr.PosZ.Value();

		float sinYaw = std::sin(Yaw);
		float cosYaw = std::cos(Yaw);

		FirstPersonCam->SetPosition(px, py + EyeHeight, pz);
		FirstPersonCam->SetYawPitch(Yaw, Pitch);

		float camDist  = 5.0f;
		float cosPitch = std::cos(Pitch);
		float tpX      = px - sinYaw * cosPitch * camDist;
		float tpY      = py + EyeHeight + std::sin(Pitch) * camDist + 1.5f;
		float tpZ      = pz + cosYaw * cosPitch * camDist;
		ThirdPersonCam->SetPosition(tpX, tpY, tpZ);
		ThirdPersonCam->SetYawPitch(Yaw, Pitch);
	}

	// Spawn position — set by game mode before Initialize is called.
	float SpawnPosX = 0.0f;
	float SpawnPosY = 5.0f;
	float SpawnPosZ = 0.0f;

	uint8_t GetOwnerID() const
	{
		Soul* s = GetOwnerSoul();
		return s ? s->GetOwnerID() : 0;
	}

private:
	/// Set the active camera only if this is the owning client (or standalone).
	void SetActiveCameraIfOwned(CameraConstruct* cam)
	{
		if (GetWorld()->GetConfig().Mode == EngineMode::Server) return;
		const uint8_t ownerID = GetOwnerID();
		// In networked mode a zero ownerID means the soul wasn't set (e.g. this
		// is a remote player's construct received via ConstructSpawn) — never
		// steal the camera for it.
		if (GetWorld()->GetConfig().Mode != EngineMode::Standalone && ownerID == 0) return;
		if (ownerID != 0 && ownerID != GetWorld()->LocalOwnerID) return;
		GetWorld()->GetLogicThread()->SetActiveCamera(cam);
	}

	CameraConstruct* ActiveCam = nullptr;

	bool bIsClientSide = false;
	EntityHandle ReplicationEntityHandle{};

	float Yaw         = 0.0f;
	float Pitch       = 0.0f;
	float DesiredVelX = 0.0f;
	float DesiredVelZ = 0.0f;
	bool bToggleHeld  = false;

	static constexpr float MoveSpeed = 1.0f;
	static constexpr float EyeHeight = 1.5f;
};
