#pragma once

#include "Construct.h"
#include "CameraConstruct.h"
#include "EInstanced.h"
#include "Input.h"
#include "JoltPhysics.h"
#include "Owned.h"

#include <cmath>

#include "EPlayer.h"
#include "JoltCharacter.h"

// ---------------------------------------------------------------------------
// PlayerConstruct — Playground player with physics capsule and dual cameras.
//
// Owns a DefaultView (capsule mesh, physics body) and two CameraConstructs
// (first-person + third-person). V key toggles the active camera.
// WASD drives kinematic velocity (set in PrePhysics, consumed by Jolt Step).
// Mouse look and camera positioning happen in ScalarUpdate (after physics).
// ---------------------------------------------------------------------------
class PlayerConstruct : public Construct<PlayerConstruct>
{
public:
	uint8_t OwnerID = 0; // NetOwnerID of the controlling Soul (0 = local/standalone)

	ConstructView<EPlayer> Body;
	Owned<CameraConstruct> FirstPersonCam;
	Owned<CameraConstruct> ThirdPersonCam;

	JoltCharacter CharacterController;

	void InitializeViews()
	{
		Body.Initialize(this);

		auto& tr = Body.Transform;
		tr.PosX  = 0.0f;
		tr.PosY  = 5.0f;
		tr.PosZ  = 0.0f;

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

		auto* phys = GetWorld()->GetPhysics();
		CharacterController.Initialize(
			phys->GetPhysicsSystem(),
			JPH::RVec3(tr.PosX.Value(), tr.PosY.Value(), tr.PosZ.Value()),
			0.3f,  // capsule radius
			0.7f); // capsule half height

		// Initialize cameras
		FirstPersonCam->Initialize(GetWorld());
		ThirdPersonCam->Initialize(GetWorld());

		// Default to third-person (better for visualizing corrections)
		ActiveCam = ThirdPersonCam.Get();
		GetWorld()->GetLogicThread()->SetActiveCamera(ActiveCam);
	}
	
	void PhysicsStep(SimFloat dt)
	{
		// Let Jolt handle collision, slopes, stairs, grounding
		CharacterController.Update(
			JPH::Vec3(DesiredVelX, 0, DesiredVelZ),
			JPH::Vec3(0, -9.81f, 0),
			static_cast<float>(dt),
			*GetWorld()->GetPhysics()->GetTempAllocator());

		// Write the resolved position back to the slab
		JPH::RVec3 pos      = CharacterController.GetPosition();
		Body.Transform.PosX = pos.GetX();
		Body.Transform.PosY = pos.GetY();
		Body.Transform.PosZ = pos.GetZ();

		// reset our desired velocity so we can begin accumulating again.
		DesiredVelX = 0.0f;
		DesiredVelZ = 0.0f;
	}

	// ── PrePhysics: read input, accumulate desired velocity ──────────────
	// Runs every logic frame (512Hz). Caches the latest desired velocity.
	// The actual Jolt push happens in PhysicsFlush (once per physics step).
	void PrePhysics(SimFloat dt)
	{
		InputBuffer* simInput = GetWorld()->GetInputForPlayer(OwnerID);

		float sinYaw = std::sin(Yaw);
		float cosYaw = std::cos(Yaw);

		float forwardX = sinYaw, forwardZ = -cosYaw;
		float rightX   = cosYaw, rightZ   = sinYaw;

		float moveX = 0.0f, moveZ = 0.0f;

		if (simInput->IsActionDown(Action::MoveForward))
		{
			moveX += forwardX;
			moveZ += forwardZ;
		}
		if (simInput->IsActionDown(Action::MoveBackward))
		{
			moveX -= forwardX;
			moveZ -= forwardZ;
		}
		if (simInput->IsActionDown(Action::MoveRight))
		{
			moveX += rightX;
			moveZ += rightZ;
		}
		if (simInput->IsActionDown(Action::MoveLeft))
		{
			moveX -= rightX;
			moveZ -= rightZ;
		}

		float len = std::sqrt(moveX * moveX + moveZ * moveZ);
		if (len > 0.001f)
		{
			DesiredVelX += moveX / len * MoveSpeed;
			DesiredVelZ += moveZ / len * MoveSpeed;
		}
	}

	// ── ScalarUpdate: mouse look, camera toggle, camera positioning ──────
	// Runs after physics — body transform is up to date.
	void ScalarUpdate(SimFloat dt)
	{
		InputBuffer* vizInput = GetWorld()->GetVizInputForPlayer(OwnerID);

		// ── Camera toggle (V key) — edge-detect ─────────────────────────
		bool toggleDown = vizInput->IsActionDown(Action::ToggleCamera);
		if (toggleDown && !bToggleHeld)
		{
			if (ActiveCam == FirstPersonCam.Get()) ActiveCam = ThirdPersonCam.Get();
			else ActiveCam                                   = FirstPersonCam.Get();

			GetWorld()->GetLogicThread()->SetActiveCamera(ActiveCam);
		}
		bToggleHeld = toggleDown;

		// ── Mouse look ──────────────────────────────────────────────────
		constexpr float MouseSens = 0.002f;
		constexpr float MaxPitch  = 1.5533f; // ~89 degrees

		Yaw   += vizInput->GetMouseDX() * MouseSens;
		Pitch -= vizInput->GetMouseDY() * MouseSens;
		if (Pitch > MaxPitch) Pitch = MaxPitch;
		if (Pitch < -MaxPitch) Pitch = -MaxPitch;

		// ── Update camera positions from body transform ─────────────────
		auto& tr    = Body.Transform;
		SimFloat px = tr.PosX.Value();
		SimFloat py = tr.PosY.Value();
		SimFloat pz = tr.PosZ.Value();

		float sinYaw = std::sin(Yaw);
		float cosYaw = std::cos(Yaw);

		// First-person: at eye height
		FirstPersonCam->SetPosition(px, py + EyeHeight, pz);
		FirstPersonCam->SetYawPitch(Yaw, Pitch);

		// Third-person: behind and above
		float camDist  = 5.0f;
		float cosPitch = std::cos(Pitch);
		float tpX      = px - sinYaw * cosPitch * camDist;
		float tpY      = py + EyeHeight + std::sin(Pitch) * camDist + 1.5f;
		float tpZ      = pz + cosYaw * cosPitch * camDist;
		ThirdPersonCam->SetPosition(tpX, tpY, tpZ);
		ThirdPersonCam->SetYawPitch(Yaw, Pitch);
	}

private:
	CameraConstruct* ActiveCam = nullptr;

	float Yaw         = 0.0f;
	float Pitch       = 0.0f;
	float DesiredVelX = 0.0f;
	float DesiredVelZ = 0.0f;
	bool bToggleHeld  = false;

	static constexpr float MoveSpeed = 1.0f;
	static constexpr float EyeHeight = 1.5f;
};
