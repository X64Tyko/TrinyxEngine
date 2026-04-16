#pragma once

#include "CameraConstruct.h"
#include "Construct.h"
#include "ConstructView.h"
#include "EngineConfig.h"
#include "Input.h"
#include "JoltCharacter.h"
#include "JoltPhysics.h"
#include "Logger.h"
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
	TNX_REGISTER_CONSTRUCT(PlayerConstruct)
	
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
			// InitialFlush sends entities with Alive-only; the Alive→Active sweep may have
			// already run by the time this entity's EntitySpawn arrives (deferred queue race).
			// Ensure Active so the predicate pass renders this entity regardless.
			Body.SetFlags(TemporalFlagBits::Active | TemporalFlagBits::Alive | TemporalFlagBits::Replicated);
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

			Body.SetFlags(TemporalFlagBits::Active | TemporalFlagBits::Alive | TemporalFlagBits::Replicated);
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
		// Two cases for client-side constructs:
		//
		// 1. Remote player (Echo) — server corrections are authoritative. Sync JoltCharacter
		//    to the ECS position (so collision shape stays in the right place) and skip input.
		//
		// 2. Local player (Owner) — predict freely with local input. Server corrections are
		//    stale by RTT; snapping to them every frame would undo the prediction. When
		//    rollback is implemented, corrections trigger a resim from the corrected frame.
		//    Until then, only snap on teleport-scale divergence (> 5 m).
		if (bIsClientSide)
		{
			const float ecsPosX = Body.Transform.PosX.Value();
			const float ecsPosY = Body.Transform.PosY.Value();
			const float ecsPosZ = Body.Transform.PosZ.Value();

			Soul* soul = GetOwnerSoul();
			if (!soul || soul->GetRole() == SoulRole::Echo)
			{
				// Remote player: drive position entirely from server-corrected ECS.
				CharacterController.SetPosition(JPH::RVec3(ecsPosX, ecsPosY, ecsPosZ));
				return;
			}

			// Local player: only teleport-snap for gross corrections (> 5 m).
			// JPH::RVec3 joltPos = CharacterController.GetPosition();
			// const float dx     = ecsPosX - static_cast<float>(joltPos.GetX());
			// const float dy     = ecsPosY - static_cast<float>(joltPos.GetY());
			// const float dz     = ecsPosZ - static_cast<float>(joltPos.GetZ());
			// if (dx * dx + dy * dy + dz * dz > 25.0f)
			{
				CharacterController.SetPosition(JPH::RVec3(ecsPosX, ecsPosY, ecsPosZ));
			}
		}

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
		// Route input through the Soul so Authority reads the injected net buffer
		// and Owner reads the local keyboard — no raw World buffer access in gameplay.
		// Standalone (no Soul, ownerID 0) falls back to the local sim buffer.
		Soul* soul            = GetOwnerSoul();
		InputBuffer* simInput = soul
			? soul->GetSimInput(GetWorld())
			: GetWorld()->GetSimInput(); // standalone fallback
		if (!simInput) return; // Echo souls have no input

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
			// PrePhysics fires PhysicsDivizor times (8×) before PhysicsStep runs once.
			// Each call contributes one 512Hz frame of movement intent. PhysicsStep
			// consumes the accumulated sum and resets it to 0, so the character covers
			// all 8 frames of intended movement in one Jolt solve.
			DesiredVelX += moveX / len * MoveSpeed;
			DesiredVelZ += moveZ / len * MoveSpeed;
		}
	}

	void ScalarUpdate(SimFloat /*dt*/)
	{
		const uint8_t ownerID = GetOwnerID();
		Soul* soul            = GetOwnerSoul();

		const bool bIsLocalPlayer = bIsClientSide
										? (soul && soul->HasRole(SoulRole::Owner))
										: (ownerID == 0); // standalone: ownerID 0 is local

		// Route through Soul when available; fall back to world buffers for
		// standalone (no Soul) and server-side remote players on the viz path.
		InputBuffer* vizInput = soul
			? soul->GetVizInput(GetWorld())
			: (bIsLocalPlayer ? GetWorld()->GetVizInput() : nullptr);

		if (!vizInput) return; // Echo or server-side remote: no viz processing

		constexpr float MouseSens = 0.002f;
		constexpr float MaxPitch  = 1.5533f; // ~89 degrees

		Yaw   += vizInput->GetMouseDX() * MouseSens;
		Pitch -= vizInput->GetMouseDY() * MouseSens;
		if (Pitch > MaxPitch) Pitch = MaxPitch;
		if (Pitch < -MaxPitch) Pitch = -MaxPitch;

		// Camera and camera-toggle are local-player-only operations.
		// Remote player constructs on the server have no cameras.
		if (!bIsLocalPlayer) return;

		bool toggleDown = vizInput->IsActionDown(Action::ToggleCamera);
		if (toggleDown && !bToggleHeld)
		{
			if (ActiveCam == FirstPersonCam.Get()) ActiveCam = ThirdPersonCam.Get();
			else ActiveCam                                   = FirstPersonCam.Get();

			SetActiveCameraIfOwned(ActiveCam);
		}
		bToggleHeld = toggleDown;

		SimFloat px, py, pz;
		// Client-side local player: read from JoltCharacter (locally predicted position,
		// not overwritten by state corrections) to avoid rubber-banding camera.
		// Server-side / standalone: Body.Transform IS the authoritative source (PhysicsStep
		// writes JoltCharacter→Body.Transform and state corrections never touch server state),
		// so reading it directly is correct and avoids any potential Jolt vs. ECS sync gap.
		if (bIsClientSide && bIsLocalPlayer)
		{
			JPH::RVec3 joltPos = CharacterController.GetPosition();
			px                 = static_cast<SimFloat>(joltPos.GetX());
			py                 = static_cast<SimFloat>(joltPos.GetY());
			pz                 = static_cast<SimFloat>(joltPos.GetZ());
		}
		else
		{
			auto& tr = Body.Transform;
			px       = tr.PosX.Value();
			py       = tr.PosY.Value();
			pz       = tr.PosZ.Value();
		}

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
		Soul* soul = GetOwnerSoul();
		// Standalone (no soul): always owns the camera.
		// Client: only the Owner soul gets the camera — not Echo, not pre-claim nulls.
		if (!soul || !soul->HasRole(SoulRole::Owner)) return;
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

	static constexpr float MoveSpeed = 1.0f;   // per-frame velocity contribution (m/s); effective speed = MoveSpeed × PhysicsDivizor (8× → 8 m/s at default ratio)
	static constexpr float EyeHeight = 1.5f;
};
