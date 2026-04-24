#pragma once

#include "CameraManager.h"
#include "Construct.h"
#include "ConstructView.h"
#include "EngineConfig.h"
#include "Input.h"
#include "JoltCharacter.h"
#include "JoltPhysics.h"
#include "Logger.h"
#include "Soul.h"

#include "EPlayer.h"

#include <cmath>

// Camera layer used by PlayerConstruct — writes eye/orbit position to WorldCameraState.
struct PlayerCameraLayer : CameraLayer, CameraStateMix<PlayerCameraLayer>
{
	float PosX = 0.f, PosY  = 0.f, PosZ = 0.f;
	float Yaw  = 0.f, Pitch = 0.f;
	float FOV  = 60.f;

	void ApplyState(WorldCameraState& state)
	{
		state.Position = {PosX, PosY, PosZ};
		state.Yaw      = Yaw;
		state.Pitch    = Pitch;
		state.FOV      = FOV;
		state.Valid    = true;
	}
};

// PlayerConstruct — Player capsule with physics character controller and dual camera layers.
class PlayerConstruct : public Construct<PlayerConstruct>
{
	TNX_REGISTER_CONSTRUCT(PlayerConstruct)

public:
	TNX_CONSTRUCT_WORLD

	ConstructView<EPlayer> Body;
	JoltCharacter CharacterController;

	void InitializeViews()
	{
		if (bIsClientSide)
		{
			Body.Attach(this, ReplicationEntityHandle);
			SpawnPosX = Body.Transform.PosX.Value();
			SpawnPosY = Body.Transform.PosY.Value();
			SpawnPosZ = Body.Transform.PosZ.Value();
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
			mesh.MeshID = 2u;

			Body.SetFlags(TemporalFlagBits::Active | TemporalFlagBits::Alive | TemporalFlagBits::Replicated);
		}

		auto* phys          = GetWorld()->GetPhysics();
		EntityRecord Record = GetWorld()->GetRegistry()->GetRecord(Body.GetEntityHandle());
		CharacterController.Initialize(
			phys,
			Record.CacheEntityIndex,
			JPH::RVec3(SpawnPosX, SpawnPosY, SpawnPosZ),
			0.3f,
			0.7f);

		Soul* soul = GetOwnerSoul();
		if (soul && soul->HasRole(SoulRole::Owner))
		{
			FPLayer.Active = false;
			TPLayer.Active = true;
			soul->GetCameraManager().AddLayer(CameraSlot::Gameplay, &FPLayer);
			soul->GetCameraManager().AddLayer(CameraSlot::Gameplay, &TPLayer);
			GetWorld()->GetLogicThread()->SetLocalCameraManager(&soul->GetCameraManager());
		}
	}

	~PlayerConstruct()
	{
		Soul* soul = GetOwnerSoul();
		if (soul && soul->HasRole(SoulRole::Owner))
		{
			soul->GetCameraManager().RemoveLayer(CameraSlot::Gameplay, &FPLayer);
			soul->GetCameraManager().RemoveLayer(CameraSlot::Gameplay, &TPLayer);
			if (IsInitialized()) GetWorld()->GetLogicThread()->SetLocalCameraManager(nullptr);
		}
	}

	void InitializeForReplication(WorldBase* world, EntityHandle* viewHandles, uint8_t viewCount)
	{
		bIsClientSide = true;
		if (viewCount > 0) ReplicationEntityHandle = viewHandles[0];
		Initialize(world);
	}

	void PhysicsStep(SimFloat dt)
	{
		if (bIsClientSide)
		{
			const float ecsPosX = Body.Transform.PosX.Value();
			const float ecsPosY = Body.Transform.PosY.Value();
			const float ecsPosZ = Body.Transform.PosZ.Value();

			Soul* soul = GetOwnerSoul();
			if (!soul || soul->GetRole() == SoulRole::Echo)
			{
				CharacterController.SetPosition(JPH::RVec3(ecsPosX, ecsPosY, ecsPosZ));
				return;
			}

			CharacterController.SetPosition(JPH::RVec3(ecsPosX, ecsPosY, ecsPosZ));
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
		Soul* soul            = GetOwnerSoul();
		InputBuffer* simInput = soul
			? soul->GetSimInput(GetWorld())
			: GetWorld()->GetSimInput();
		if (!simInput) return;

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

	void PostPhysics(SimFloat /*dt*/)
	{
		const uint8_t ownerID = GetOwnerID();
		Soul* soul            = GetOwnerSoul();

		const bool bIsLocalPlayer = bIsClientSide
										? (soul && soul->HasRole(SoulRole::Owner))
										: (ownerID == 0);

		InputBuffer* vizInput = soul
			? soul->GetVizInput(GetWorld())
			: (bIsLocalPlayer ? GetWorld()->GetVizInput() : nullptr);

		if (!vizInput) return;

		constexpr float MouseSens = 0.002f;
		constexpr float MaxPitch  = 1.5533f;

		Yaw   += vizInput->GetMouseDX() * MouseSens;
		Pitch -= vizInput->GetMouseDY() * MouseSens;
		if (Pitch > MaxPitch) Pitch = MaxPitch;
		if (Pitch < -MaxPitch) Pitch = -MaxPitch;

		if (!bIsLocalPlayer) return;

		bool toggleDown = vizInput->IsActionDown(Action::ToggleCamera);
		if (toggleDown && !bToggleHeld)
		{
			FPLayer.Active = !FPLayer.Active;
			TPLayer.Active = !TPLayer.Active;
		}
		bToggleHeld = toggleDown;

		// Local player: use Jolt position to avoid camera rubber-banding.
		SimFloat px, py, pz;
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

		float sinYaw   = std::sin(Yaw);
		float cosYaw   = std::cos(Yaw);
		float cosPitch = std::cos(Pitch);

		FPLayer.PosX  = px;
		FPLayer.PosY  = py + EyeHeight;
		FPLayer.PosZ  = pz;
		FPLayer.Yaw   = Yaw;
		FPLayer.Pitch = Pitch;

		constexpr float CamDist = 5.0f;
		TPLayer.PosX            = px - sinYaw * cosPitch * CamDist;
		TPLayer.PosY            = py + EyeHeight + std::sin(Pitch) * CamDist + 1.5f;
		TPLayer.PosZ            = pz + cosYaw * cosPitch * CamDist;
		TPLayer.Yaw             = Yaw;
		TPLayer.Pitch           = Pitch;
	}

	float SpawnPosX = 0.0f;
	float SpawnPosY = 5.0f;
	float SpawnPosZ = 0.0f;

	uint8_t GetOwnerID() const
	{
		Soul* s = GetOwnerSoul();
		return s ? s->GetOwnerID() : 0;
	}

private:
	PlayerCameraLayer FPLayer; // first-person
	PlayerCameraLayer TPLayer; // third-person, default active

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
