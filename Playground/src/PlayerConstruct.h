#pragma once

#include "Construct.h"
#include "CameraConstruct.h"
#include "EInstanced.h"
#include "Input.h"
#include "JoltPhysics.h"
#include "Owned.h"

#include <cmath>

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
	ConstructView<EInstanced> Body;
	Owned<CameraConstruct> FirstPersonCam;
	Owned<CameraConstruct> ThirdPersonCam;

	void InitializeViews()
	{
		Body.Initialize(this);

		auto& tr = Body.Transform;
		tr.PosX  = 0.0f;
		tr.PosY  = 5.0f;
		tr.PosZ  = 0.0f;

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

		auto& jolt       = Body.PhysBody;
		jolt.Shape       = JoltShapeType::Capsule;
		jolt.HalfExtentX = 0.4f; // radius
		jolt.HalfExtentY = 0.9f; // half-height
		jolt.Motion      = JoltMotion::Kinematic;
		jolt.Mass        = 80.0f;
		jolt.Friction    = 0.5f;
		jolt.Restitution = 0.1f;

		Body.SetFlags(TemporalFlagBits::Active | TemporalFlagBits::Replicated);

		// Initialize cameras
		FirstPersonCam->Initialize(GetWorld());
		ThirdPersonCam->Initialize(GetWorld());

		// Default to third-person (better for visualizing corrections)
		ActiveCam = ThirdPersonCam.Get();
		GetWorld()->GetLogicThread()->SetActiveCamera(ActiveCam);
	}

	// ── PrePhysics: read input, accumulate desired velocity ──────────────
	// Runs every logic frame (512Hz). Caches the latest desired velocity.
	// The actual Jolt push happens in PhysicsFlush (once per physics step).
	void PrePhysics(SimFloat dt)
	{
		InputBuffer* simInput = GetWorld()->GetSimInput();

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
			DesiredVelX = moveX / len * MoveSpeed;
			DesiredVelZ = moveZ / len * MoveSpeed;
		}
		else
		{
			DesiredVelX = 0.0f;
			DesiredVelZ = 0.0f;
		}
	}

	// ── ScalarUpdate: mouse look, camera toggle, camera positioning ──────
	// Runs after physics — body transform is up to date.
	void ScalarUpdate(SimFloat dt)
	{
		InputBuffer* vizInput = GetWorld()->GetVizInput();

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

	static constexpr float MoveSpeed = 8.0f;
	static constexpr float EyeHeight = 1.5f;
};
