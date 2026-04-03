#pragma once

#include "Construct.h"
#include "ConstructView.h"
#include "EInstanced.h"
#include "EPlayer.h"
#include "JoltCharacter.h"
#include "JoltPhysics.h"

// ---------------------------------------------------------------------------
// PlayerConstruct — Proof-of-concept Construct with a DefaultView.
//
// Spawns a visible physics cube and moves it with keyboard input each frame.
// Demonstrates the full Construct stack: View creation, tick auto-registration,
// scalar field access from a Construct's ScalarUpdate.
// ---------------------------------------------------------------------------
class PlayerConstruct : public Construct<PlayerConstruct>
{
	ConstructView<EPlayer> Body;
	JoltCharacter CharacterController;

public:
	void InitializeViews()
	{
		Body.Initialize(this);

		auto& tr = Body.Transform;
		tr.PosX  = 0.0f;
		tr.PosY  = 5.0f;
		tr.PosZ  = -60.0f;
		tr.Rotation.SetIdentity();

		auto& sc  = Body.Scale;
		sc.ScaleX = 3.0f;
		sc.ScaleY = 3.0f;
		sc.ScaleZ = 3.0f;

		auto& col = Body.Color;
		col.R     = 0.2f;
		col.G     = 1.0f;
		col.B     = 0.2f;
		col.A     = 1.0f;

		Body.SetFlags(TemporalFlagBits::Active | TemporalFlagBits::Replicated);

		Body.Mesh.MeshID = 1u; // Capsule (slot 0=Cube, slot 1=Capsule)

		// Set up the character controller — not a JoltBody, separate system
		auto* phys = GetWorld()->GetPhysics();
		CharacterController.Initialize(
			phys->GetPhysicsSystem(),
			JPH::RVec3(tr.PosX.Value(), tr.PosY.Value(), tr.PosZ.Value()),
			0.3f,  // capsule radius
			0.7f); // capsule half height
	}
	
	void PhysicsStep(SimFloat dt)
	{
		// Simple oscillation to prove the tick is running
		static float Timer = 0.0f;
		Timer              += dt;
	
		// Let Jolt handle collision, slopes, stairs, grounding
		CharacterController.Update(
			JPH::Vec3(0, 0, 0),
			JPH::Vec3(0, -9.81f, 0),
			static_cast<float>(dt),
			*GetWorld()->GetPhysics()->GetTempAllocator());

		// Write the resolved position back to the slab
		JPH::RVec3 pos      = CharacterController.GetPosition();
		Body.Transform.PosX = pos.GetX();
		Body.Transform.PosY = pos.GetY();
		Body.Transform.PosZ = pos.GetZ();

		// Pulse color to visually confirm PrePhysics is live
		auto& col = Body.Color;
		col.G     = 0.5f + 0.5f * std::sin(Timer * 3.0f);
	}
};
