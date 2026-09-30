#include "PlayerConstruct.h"

#include "EngineConfig.h"
#include "Input.h"
#include "JoltPhysics.h"
#include "Logger.h"
#include "QuatMath.h"
#include "Soul.h"

#include <cmath>

void PlayerCameraLayer::ApplyState(WorldCameraState& state)
{
	state.Position = { PosX, PosY, PosZ };
	state.Rotation = QuatFromYawPitch(Yaw, Pitch);
	state.FOV      = FOV;
	state.Valid    = true;
}

void PlayerConstruct::InitializeViews()
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
	Vector3 spawnPos{ SpawnPosX, SpawnPosY, SpawnPosZ };
	Body.SetPosition(spawnPos);
	Body.VisTransform.VisBlend = SimFloat(0.6f);

	auto* phys = GetWorld()->GetPhysics();
	CharacterController.Initialize(
		phys,
		JPH::RVec3(SpawnPosX.ToFloat(), SpawnPosY.ToFloat(), SpawnPosZ.ToFloat()),
		0.3f,
		0.7f);

	phys->BindConstructOnHit<PlayerConstruct, &PlayerConstruct::OnHit>(
		CharacterController.GetInnerBodyID(), this, Body.GetEntityHandle());

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

PlayerConstruct::~PlayerConstruct()
{
	Soul* soul = GetOwnerSoul();
	if (soul && soul->HasRole(SoulRole::Owner))
	{
		soul->GetCameraManager().RemoveLayer(CameraSlot::Gameplay, &FPLayer);
		soul->GetCameraManager().RemoveLayer(CameraSlot::Gameplay, &TPLayer);
		if (IsInitialized() && GetWorld() && GetWorld()->GetLogicThread()) GetWorld()->GetLogicThread()->SetLocalCameraManager(nullptr);
	}
}

void PlayerConstruct::InitializeForReplication(WorldBase* world, EntityHandle* viewHandles, uint8_t viewCount)
{
	bIsClientSide = true;
	if (viewCount > 0) ReplicationEntityHandle = viewHandles[0];
	Initialize(world);
}

void PlayerConstruct::PrePhysics(SimFloat dt)
{
	Soul* soul            = GetOwnerSoul();
	InputBuffer* simInput = soul
								? soul->GetSimInput(GetWorld())
								: GetWorld()->GetSimInput();
	if (!simInput) return;

	// Facing comes only from this frame's sim input: identical on the Owner, the Authority, and every
	// resimulated frame. Never derive it from viz input or a Construct member.
	const SimFloat yaw   = simInput->GetViewYaw();
	const SimFloat pitch = simInput->GetViewPitch();
	if (Body.ControlRotation.Yaw.Value() != yaw) Body.ControlRotation.Yaw = yaw;
	if (Body.ControlRotation.Pitch.Value() != pitch) Body.ControlRotation.Pitch = pitch;

	SimFloat sinYaw = FastSin(yaw);
	SimFloat cosYaw = FastCos(yaw);

	SimFloat forwardX = sinYaw, forwardZ = -cosYaw;
	SimFloat rightX = cosYaw, rightZ = sinYaw;

	SimFloat moveX = 0.0f, moveZ = 0.0f;

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

	SimFloat len   = Sqrt(moveX * moveX + moveZ * moveZ);
	SimFloat XDelt = 0;
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

void PlayerConstruct::PhysicsStep(SimFloat dt)
{
	// if (bIsClientSide)
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

	JPH::RVec3 pos  = CharacterController.GetPosition();
	PhysPos         = Vector3(pos.GetX(), pos.GetY(), pos.GetZ());
	Vector3 BodyPos = { Body.Transform.PosX.Value(), Body.Transform.PosY.Value(), Body.Transform.PosZ.Value() };
	if ((BodyPos - PhysPos).LengthSqr() > SimFloat(0.0003f))
	{
		Vector3 tempPos = { pos.GetX(), pos.GetY(), pos.GetZ() };
		Body.SetPosition(tempPos);
	}

	/*
	if (GetOwnerSoul()->HasRole(SoulRole::Authority))
		LOG_NET_INFO_F(GetOwnerSoul(), "PlayerConstruct::PhysStep: PosX: %u, PosY: %u, PosZ: %u", Body.Transform.PosX.Value().ToFixed(), Body.Transform.PosY.Value().ToFixed(), Body.Transform.PosZ.Value().ToFixed());
		*/

	DesiredVelX = 0.0f;
	DesiredVelZ = 0.0f;
}

void PlayerConstruct::OnHit(PhysicsOnHitData /*data*/) {}

void PlayerConstruct::PostPhysics(SimFloat /*dt*/)
{
	const uint8_t ownerID = GetOwnerID();
	Soul* soul            = GetOwnerSoul();

	const bool bIsLocalPlayer = bIsClientSide
									? (soul && soul->HasRole(SoulRole::Owner))
									: (ownerID == 0);

	// Only the local Owner turns mouse motion into view angles. Everyone else (the Authority for
	// remote players, Echoes) takes facing from sim input / CControlRotation.
	if (!bIsLocalPlayer) return;

	InputBuffer* vizInput = soul ? soul->GetVizInput(GetWorld()) : GetWorld()->GetVizInput();
	if (!vizInput) return;

	// Resimulated frames replay their recorded view angles; integrating viz input again would
	// re-apply mouse motion that already happened.
	if (!GetWorld()->GetLogicThread()->IsResimulating())
	{
		constexpr SimFloat MouseSens = SimFloat(0.002f);
		constexpr SimFloat MaxPitch  = SimFloat(1.5533f);

		Yaw += vizInput->GetMouseDX() * MouseSens;
		Pitch -= vizInput->GetMouseDY() * MouseSens;
		if (Pitch > MaxPitch) Pitch = MaxPitch;
		if (Pitch < -MaxPitch) Pitch = -MaxPitch;

		// Hand the view to the next sim frame; it travels in the input snapshot to the Authority.
		InputBuffer* simInput = soul ? soul->GetSimInput(GetWorld()) : GetWorld()->GetSimInput();
		if (simInput) simInput->SetViewAngles(Yaw, Pitch);
	}

	bool toggleDown = vizInput->IsActionDown(Action::ToggleCamera);
	if (toggleDown && !bToggleHeld)
	{
		FPLayer.Active = !FPLayer.Active;
		TPLayer.Active = !TPLayer.Active;
	}
	bToggleHeld = toggleDown;

	SimFloat px, py, pz;
	px = Body.VisTransform.VisPosX.Value();
	py = Body.VisTransform.VisPosY.Value();
	pz = Body.VisTransform.VisPosZ.Value();

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

uint8_t PlayerConstruct::GetOwnerID() const
{
	Soul* s = GetOwnerSoul();
	return s ? s->GetOwnerID() : 0;
}
