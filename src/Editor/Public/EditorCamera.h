#pragma once
#if !defined(TNX_ENABLE_EDITOR)
#error "EditorCamera.h requires TNX_ENABLE_EDITOR"
#endif

#include "Types.h"

struct TemporalFrameHeader;

// -----------------------------------------------------------------------
// ViewCamera — the resolved camera one viewport renders with, in render-space floats.
//
// Every consumer of a viewport's camera (GPU frame data, gizmo, grid overlay) goes through
// ResolveViewCamera so they can never disagree about where the viewport is looking.
// -----------------------------------------------------------------------
struct ViewCamera
{
	float Position[3]{};
	Quatf Rotation{};
	float FoVDeg = 60.0f;

	// Previous pose for GPU interpolation. Equal to the current pose when the camera is
	// updated at render rate (editor camera) rather than once per fixed step.
	float PrevPosition[3]{};
	Quatf PrevRotation{};
	float PrevFoVDeg = 60.0f;
};

/// Per-frame input for EditorCamera::Fly. Mouse deltas are relative motion in pixels.
struct EditorCameraInput
{
	float MouseDX = 0.0f;
	float MouseDY = 0.0f;
	float Wheel   = 0.0f;
	bool Forward  = false;
	bool Back     = false;
	bool Left     = false;
	bool Right    = false;
	bool Up       = false;
	bool Down     = false;
	bool Boost    = false;
};

// -----------------------------------------------------------------------
// EditorCamera — a viewport-owned camera, updated on the render thread.
//
// Presentation state, not simulation state: it never enters the slab, the temporal ring, or
// undo history, and moving it costs the Brain nothing. Yaw/pitch follow the engine camera
// convention (yaw about world +Y, pitch about local +X, yaw = pitch = 0 faces -Z).
// -----------------------------------------------------------------------
class EditorCamera
{
public:
	/// Free-fly: mouse look, WASD/QE translation, wheel scales move speed.
	void Fly(const EditorCameraInput& input, float dt);

	/// Place the camera explicitly.
	void SetPose(float x, float y, float z, float yaw, float pitch);

	/// Adopt the world's published camera the first time the viewport is used, so switching
	/// to the editor camera doesn't teleport the view.
	void SeedFrom(const TemporalFrameHeader& header);

	bool IsSeeded() const { return bSeeded; }
	float GetMoveSpeed() const { return MoveSpeed; }
	ViewCamera GetView() const;

private:
	float Position[3]{};
	float Yaw       = 0.0f;
	float Pitch     = 0.0f;
	float FoVDeg    = 60.0f;
	float MoveSpeed = 20.0f; ///< Units per second; wheel scales it geometrically.
	bool bSeeded    = false;
};

struct WorldViewport;

/// The camera a viewport renders with: its EditorCamera when enabled, otherwise the world's
/// published camera from the given frame header.
ViewCamera ResolveViewCamera(const WorldViewport& viewport, const TemporalFrameHeader& header);
