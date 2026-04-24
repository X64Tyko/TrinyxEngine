#pragma once

#include "Construct.h"
#include "ConstructView.h"
#include "EInstanced.h"
#include "JoltLayers.h"
#include "PhysicsEvents.h"

// ---------------------------------------------------------------------------
// TriggerVolume — a Playground Construct wrapping a Jolt sensor body.
//
// Usage:
//   1. Set PosX/Y/Z and HalfX/Y/Z BEFORE Initialize (via ConstructRegistry::Create config param).
//   2. Bind OnEnter / OnExit callbacks (also before or inside OnSpawned).
//
// The sensor body is a real ECS entity (ConstructView<EInstanced>) with
// CJoltBody::IsSensor = 1. FlushPendingBodies creates it as a Jolt sensor,
// and Construct auto-binding wires OnOverlapBegin/OnOverlapEnd automatically.
// ---------------------------------------------------------------------------
class TriggerVolume : public Construct<TriggerVolume>
{
public:
	TNX_CONSTRUCT_WORLD

	// --- Configuration (set before Initialize via the config lambda) ---
	float PosX  = 0.f, PosY  = 0.f, PosZ  = 0.f;
	float HalfX = 1.f, HalfY = 1.f, HalfZ = 1.f;

	// --- User-bindable callbacks ---
	using TriggerCallback = Callback<void, PhysicsOverlapData>;
	TriggerCallback OnEnter;
	TriggerCallback OnExit;

	void InitializeViews()
	{
		Body.Initialize(this, [this](EInstanced<>& v)
		{
			v.SetFlags(TemporalFlagBits::Active | TemporalFlagBits::Alive | TemporalFlagBits::Replicated);
			v.Transform.PosX = PosX;
			v.Transform.PosY = PosY;
			v.Transform.PosZ = PosZ;
			v.Transform.Rotation.SetIdentity();
			v.Scale.ScaleX         = HalfX * 2.0f;
			v.Scale.ScaleY         = HalfY * 2.0f;
			v.Scale.ScaleZ         = HalfZ * 2.0f;
			v.Color.R              = 0.2f;
			v.Color.G              = 0.8f;
			v.Color.B              = 0.2f;
			v.Color.A              = 0.15f;
			v.Mesh.MeshID          = 1u;
			v.PhysBody.Shape       = JoltShapeType::Box;
			v.PhysBody.HalfExtentX = HalfX;
			v.PhysBody.HalfExtentY = HalfY;
			v.PhysBody.HalfExtentZ = HalfZ;
			v.PhysBody.Motion      = JoltMotion::Kinematic;
			v.PhysBody.IsSensor    = 1u;
		});
	}

	// --- Contact concept hooks (auto-bound by Construct<T>::Initialize) ---

	void OnOverlapBegin(PhysicsOverlapData data)
	{
		if (OnEnter.IsBound()) OnEnter(data);
	}

	void OnOverlapEnd(PhysicsOverlapData data)
	{
		if (OnExit.IsBound()) OnExit(data);
	}

private:
	ConstructView<EInstanced> Body;
};
