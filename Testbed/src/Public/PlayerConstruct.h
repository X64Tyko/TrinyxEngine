#pragma once

#include "Construct.h"
#include "DefaultView.h"
#include "Input.h"

// ---------------------------------------------------------------------------
// PlayerConstruct — Proof-of-concept Construct with a DefaultView.
//
// Spawns a visible physics cube and moves it with keyboard input each frame.
// Demonstrates the full Construct stack: View creation, tick auto-registration,
// scalar field access from a Construct's ScalarUpdate.
// ---------------------------------------------------------------------------
class PlayerConstruct : public Construct<PlayerConstruct>
{
public:
	DefaultView Body;

	void InitializeViews()
	{
		Body.Initialize(GetRegistry());

		auto& tr = Body.Get<TransRot>();
		tr.PosX  = 0.0f;
		tr.PosY  = 5.0f;
		tr.PosZ  = -60.0f;

		auto& sc  = Body.Get<Scale>();
		sc.ScaleX = 1.0f;
		sc.ScaleY = 1.0f;
		sc.ScaleZ = 1.0f;

		auto& col = Body.Get<ColorData>();
		col.R     = 0.2f;
		col.G     = 1.0f;
		col.B     = 0.2f;
		col.A     = 1.0f;

		auto& jolt       = Body.Get<JoltBody>();
		jolt.Shape       = JoltShapeType::Capsule;
		jolt.HalfExtentX = 0.4f; // radius
		jolt.HalfExtentY = 0.9f; // half-height
		jolt.Motion      = JoltMotion::Dynamic;
		jolt.Mass        = 80.0f;
		jolt.Friction    = 0.5f;
		jolt.Restitution = 0.1f;
	}

	void PrePhysics(SimFloat dt)
	{
		/* TODO: Currently this fights with Jolt, we don't have a way to re-push.
		 * I've got a few ideas about kinematic and marking entities to constant sync,
		 * but there's also the issue of the PhysDivisor to tackle. for now this proves
		 * out the construct system lifetime functions are working.
		 */
		auto& tr = Body.Get<TransRot>();

		// Simple oscillation to prove the tick is running
		static float Timer = 0.0f;
		Timer              += dt;
		tr.PosX            = std::sin(Timer) * 10.0f;

		// Pulse color to visually confirm PrePhysics is live
		auto& col = Body.Get<ColorData>();
		col.G     = 0.5f + 0.5f * std::sin(Timer * 3.0f);
	}
};
