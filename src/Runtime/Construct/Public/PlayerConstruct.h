#pragma once

#include "CameraManager.h"
#include "QuatMath.h"
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
	SimFloat PosX = SimFloat(0.f), PosY  = SimFloat(0.f), PosZ = SimFloat(0.f);
	SimFloat Yaw  = SimFloat(0.f), Pitch = SimFloat(0.f);
	SimFloat FOV  = SimFloat(60.f);

	void ApplyState(WorldCameraState& state)
	{
		state.Position = {PosX, PosY, PosZ};
		state.Rotation = QuatFromYawPitch(Yaw, Pitch);
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
	Vector3 PhysPos;

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
			sc.ScaleX = SimFloat(1.0f);
			sc.ScaleY = SimFloat(1.0f);
			sc.ScaleZ = SimFloat(1.0f);

			auto& col = Body.Color;
			col.R     = SimFloat(0.2f);
			col.G     = SimFloat(0.8f);
			col.B     = SimFloat(0.2f);
			col.A     = SimFloat(1.0f);

			auto& mesh  = Body.Mesh;
			mesh.MeshID = 2u;

			Body.SetFlags(TemporalFlagBits::Active | TemporalFlagBits::Alive | TemporalFlagBits::Replicated);
		}

		// Seed visual position to match the authoritative spawn position so the first
		// frame doesn't blend from (0,0,0).
		Body.VisTransform.VisPosX = SpawnPosX;
		Body.VisTransform.VisPosY = SpawnPosY;
		Body.VisTransform.VisPosZ = SpawnPosZ;
		Body.VisTransform.VisBlend = SimFloat(0.2f);

		auto* phys          = GetWorld()->GetPhysics();
		EntityRecord Record = GetWorld()->GetRegistry()->GetRecord(Body.GetEntityHandle());
		CharacterController.Initialize(
			phys,
			Record.CacheEntityIndex,
			JPH::RVec3(SpawnPosX.ToFloat(), SpawnPosY.ToFloat(), SpawnPosZ.ToFloat()),
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
			if (IsInitialized() && GetWorld() && GetWorld()->GetLogicThread()) GetWorld()->GetLogicThread()->SetLocalCameraManager(nullptr);
		}
	}

	void InitializeForReplication(WorldBase* world, EntityHandle* viewHandles, uint8_t viewCount)
	{
		bIsClientSide = true;
		if (viewCount > 0) ReplicationEntityHandle = viewHandles[0];
		Initialize(world);
	}

	void PrePhysics(SimFloat dt)
	{
		Soul* soul            = GetOwnerSoul();
		InputBuffer* simInput = soul
			? soul->GetSimInput(GetWorld())
			: GetWorld()->GetSimInput();
		if (!simInput) return;

		SimFloat sinYaw = FastSin(Yaw);
		SimFloat cosYaw = FastCos(Yaw);

		SimFloat forwardX = sinYaw, forwardZ = -cosYaw;
		SimFloat rightX   = cosYaw, rightZ   = sinYaw;

		SimFloat moveX = 0.0f, moveZ = 0.0f;

		if (simInput->IsActionDown(Action::MoveForward))  { moveX += forwardX; moveZ += forwardZ; }
		if (simInput->IsActionDown(Action::MoveBackward)) { moveX -= forwardX; moveZ -= forwardZ; }
		if (simInput->IsActionDown(Action::MoveRight))    { moveX += rightX;   moveZ += rightZ;   }
		if (simInput->IsActionDown(Action::MoveLeft))     { moveX -= rightX;   moveZ -= rightZ;   }

		SimFloat len = Sqrt(moveX * moveX + moveZ * moveZ);
		SimFloat XDelt =  0;
		SimFloat ZDelt = 0;
		if (len > 0.f)
		{
			XDelt = moveX / len * MoveSpeed * dt;
			ZDelt = moveZ / len * MoveSpeed * dt;
			Body.Transform.PosX += XDelt;
			Body.Transform.PosZ += ZDelt;
			Body.VisTransform.VisPosX += XDelt;
			Body.VisTransform.VisPosZ += ZDelt;
			DesiredVelX += XDelt;
			DesiredVelZ += ZDelt;
		}
		
		/*
		if (GetOwnerSoul()->HasRole(SoulRole::Authority))
			LOG_NET_INFO_F(GetOwnerSoul(), "PlayerConstruct::ProcessInput: PosX: %u, PosY: %u, PosZ: %u, Delta: %f, %f", Body.Transform.PosX.Value().ToFixed(), Body.Transform.PosY.Value().ToFixed(), Body.Transform.PosZ.Value().ToFixed(), XDelt.ToFloat(), ZDelt.ToFloat());
			*/
	}

	void PhysicsStep(SimFloat dt)
	{
		//if (bIsClientSide)
		{
			// Set position to corrected position - our desired velocity.
			const SimFloat ecsPosX = Body.Transform.PosX.Value() - DesiredVelX;
			const SimFloat ecsPosY = Body.Transform.PosY.Value();
			const SimFloat ecsPosZ = Body.Transform.PosZ.Value() - DesiredVelZ;
			
			CharacterController.SetPosition(JPH::RVec3(ecsPosX.ToFloat(), ecsPosY.ToFloat(), ecsPosZ.ToFloat()));
			
			Soul* soul = GetOwnerSoul();
			if (!soul || soul->GetRole() == SoulRole::Echo)
			{
				return;
			}
		}

		CharacterController.Update(
			JPH::Vec3((DesiredVelX / dt).ToFloat(), 0, (DesiredVelZ / dt).ToFloat()),
			JPH::Vec3(0, -9.81f, 0),
			dt.ToFloat(),
			*GetWorld()->GetPhysics()->GetTempAllocator());

		JPH::RVec3 pos      = CharacterController.GetPosition();
		PhysPos = Vector3(pos.GetX(), pos.GetY(), pos.GetZ());
		Vector3 BodyPos = { Body.Transform.PosX.Value(), Body.Transform.PosY.Value(), Body.Transform.PosZ.Value() };
		if ((BodyPos - PhysPos).LengthSqr() > SimFloat(0.0003f))
		{
			Vector3 tempPos = {pos.GetX(), pos.GetY(), pos.GetZ()};
			Body.SetPosition(tempPos);
		}
		
		/*
		if (GetOwnerSoul()->HasRole(SoulRole::Authority))
			LOG_NET_INFO_F(GetOwnerSoul(), "PlayerConstruct::PhysStep: PosX: %u, PosY: %u, PosZ: %u", Body.Transform.PosX.Value().ToFixed(), Body.Transform.PosY.Value().ToFixed(), Body.Transform.PosZ.Value().ToFixed());
			*/

		DesiredVelX = 0.0f;
		DesiredVelZ = 0.0f;
		
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

		constexpr SimFloat MouseSens = SimFloat(0.002f);
		constexpr SimFloat MaxPitch  = SimFloat(1.5533f);

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

		SimFloat px, py, pz;
		px                 = Body.VisTransform.VisPosX.Value();
		py                 = Body.VisTransform.VisPosY.Value();
		pz                 = Body.VisTransform.VisPosZ.Value();

		SimFloat sinYaw   = FastSin(Yaw);
		SimFloat cosYaw   = FastCos(Yaw);
		SimFloat cosPitch = FastCos(Pitch);

		FPLayer.PosX  = px;
		FPLayer.PosY  = py + EyeHeight;
		FPLayer.PosZ  = pz;
		FPLayer.Yaw   = Yaw;
		FPLayer.Pitch = Pitch;

		constexpr SimFloat CamDist = SimFloat(5.0f);
		TPLayer.PosX               = px - sinYaw * cosPitch * CamDist;
		TPLayer.PosY               = py + EyeHeight + FastSin(Pitch) * CamDist + SimFloat(1.5f);
		TPLayer.PosZ               = pz + cosYaw * cosPitch * CamDist;
		TPLayer.Yaw                = Yaw;
		TPLayer.Pitch              = Pitch;
	}

	SimFloat SpawnPosX = SimFloat(0.0f);
	SimFloat SpawnPosY = SimFloat(5.0f);
	SimFloat SpawnPosZ = SimFloat(0.0f);

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

	SimFloat Yaw         = SimFloat(0.0f);
	SimFloat Pitch       = SimFloat(0.0f);
	SimFloat DesiredVelX = SimFloat(0.0f);
	SimFloat DesiredVelZ = SimFloat(0.0f);
	bool bToggleHeld     = false;

	static constexpr SimFloat MoveSpeed = SimFloat(8.0f);
	static constexpr SimFloat EyeHeight = SimFloat(1.5f);
};
