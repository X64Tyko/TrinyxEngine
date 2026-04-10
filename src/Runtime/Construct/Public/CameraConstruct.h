#pragma once

#include "Construct.h"
#include "ConstructView.h"
#include "EPoint.h"

// ---------------------------------------------------------------------------
// CameraConstruct — Camera as an in-world Construct.
//
// Owns a LogicView (transform only, LOGIC partition — no physics, no render).
// Stores yaw/pitch/FOV as scalar C++ state. The LogicThread reads the active
// camera's transform to build the ViewMatrix each frame.
//
// Forward-compatible: future work adds RenderView for editor gizmo
// visualization. Multiple cameras work naturally by swapping the active
// pointer on LogicThread.
//
// The CameraConstruct is a positioned lens. Offset and behavior (first-person
// eye offset, third-person boom arm) are the caller's responsibility.
// ---------------------------------------------------------------------------
class CameraConstruct : public Construct<CameraConstruct>
{
	ConstructView<EPoint> Transform;

public:
	void InitializeViews()
	{
		Transform.Initialize(this);
	}

	void SetPosition(SimFloat x, SimFloat y, SimFloat z)
	{
		auto& tr          = Transform;
		tr.Transform.PosX = x;
		tr.Transform.PosY = y;
		tr.Transform.PosZ = z;
	}

	void GetPosition(SimFloat& x, SimFloat& y, SimFloat& z)
	{
		auto& tr = Transform;
		x        = tr.Transform.PosX.Value();
		y        = tr.Transform.PosY.Value();
		z        = tr.Transform.PosZ.Value();
	}

	void SetYawPitch(float yaw, float pitch)
	{
		Yaw   = yaw;
		Pitch = pitch;
	}

	float GetYaw() const { return Yaw; }
	float GetPitch() const { return Pitch; }
	float GetFOV() const { return FOV; }
	void SetFOV(float fov) { FOV = fov; }

private:
	float Yaw   = 0.0f;
	float Pitch = 0.0f;
	float FOV   = 60.0f;
};