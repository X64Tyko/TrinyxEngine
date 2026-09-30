#include "EditorCamera.h"

#include <algorithm>
#include <cmath>

#include "TemporalComponentCache.h"
#include "WorldViewport.h"

namespace
{
constexpr float kMouseSens  = 0.002f;  // rad/pixel
constexpr float kMaxPitch   = 1.5533f; // ~89 degrees
constexpr float kBoostScale = 4.0f;
constexpr float kMinSpeed   = 0.25f;
constexpr float kMaxSpeed   = 2000.0f;

// Float mirror of QuatFromYawPitch: positive yaw turns right (-yaw about +Y), then pitch about local +X.
Quatf QuatfFromYawPitch(float yaw, float pitch)
{
	const float hy = yaw * 0.5f;
	const float hp = pitch * 0.5f;
	const Quatf qY{ 0.0f, -std::sin(hy), 0.0f, std::cos(hy) };
	const Quatf qP{ std::sin(hp), 0.0f, 0.0f, std::cos(hp) };
	return qY * qP;
}
} // namespace

void EditorCamera::Fly(const EditorCameraInput& input, float dt)
{
	// Same convention as the rest of the engine (see QuatFromYawPitch): mouse right turns right,
	// and the move basis is exactly the basis the view renders with.
	Yaw += input.MouseDX * kMouseSens;
	Pitch = std::clamp(Pitch - input.MouseDY * kMouseSens, -kMaxPitch, kMaxPitch);

	if (input.Wheel != 0.0f) MoveSpeed = std::clamp(MoveSpeed * std::pow(1.2f, input.Wheel), kMinSpeed, kMaxSpeed);

	const float sinYaw = std::sin(Yaw), cosYaw = std::cos(Yaw);
	const float sinPitch = std::sin(Pitch), cosPitch = std::cos(Pitch);
	const Vector3f forward{ sinYaw * cosPitch, sinPitch, -cosYaw * cosPitch };
	const Vector3f right{ cosYaw, 0.0f, sinYaw };

	Vector3f move{ 0.0f, 0.0f, 0.0f };
	if (input.Forward) move = move + forward;
	if (input.Back) move = move - forward;
	if (input.Right) move = move + right;
	if (input.Left) move = move - right;
	if (input.Up) move.y = move.y + 1.0f;
	if (input.Down) move.y = move.y - 1.0f;

	const float len = std::sqrt(move.x * move.x + move.y * move.y + move.z * move.z);
	if (len > 0.001f)
	{
		const float step = dt * MoveSpeed * (input.Boost ? kBoostScale : 1.0f) / len;
		Position[0] += move.x * step;
		Position[1] += move.y * step;
		Position[2] += move.z * step;
	}
	bSeeded = true;
}

void EditorCamera::SetPose(float x, float y, float z, float yaw, float pitch)
{
	Position[0] = x;
	Position[1] = y;
	Position[2] = z;
	Yaw         = yaw;
	Pitch       = std::clamp(pitch, -kMaxPitch, kMaxPitch);
	bSeeded     = true;
}

void EditorCamera::SeedFrom(const TemporalFrameHeader& header)
{
	Position[0] = header.CameraPosition.x.ToFloat();
	Position[1] = header.CameraPosition.y.ToFloat();
	Position[2] = header.CameraPosition.z.ToFloat();

	// Recover yaw/pitch from the published forward vector (roll is not representable here).
	const Vector3f forward = header.CameraRotation.ToFloat().Rotate(Vector3f{ 0.0f, 0.0f, -1.0f });
	Pitch                  = std::clamp(std::asin(std::clamp(forward.y, -1.0f, 1.0f)), -kMaxPitch, kMaxPitch);
	Yaw                    = std::atan2(forward.x, -forward.z);

	const float fov = header.CameraFoV.ToFloat();
	if (fov > 1.0f) FoVDeg = fov;
	bSeeded = true;
}

ViewCamera EditorCamera::GetView() const
{
	ViewCamera view;
	view.Position[0]     = Position[0];
	view.Position[1]     = Position[1];
	view.Position[2]     = Position[2];
	view.Rotation        = QuatfFromYawPitch(Yaw, Pitch);
	view.FoVDeg          = FoVDeg;
	view.PrevPosition[0] = Position[0];
	view.PrevPosition[1] = Position[1];
	view.PrevPosition[2] = Position[2];
	view.PrevRotation    = view.Rotation;
	view.PrevFoVDeg      = FoVDeg;
	return view;
}

ViewCamera ResolveViewCamera(const WorldViewport& viewport, const TemporalFrameHeader& header)
{
	if (viewport.bUseEditorCamera && viewport.Camera.IsSeeded()) return viewport.Camera.GetView();

	ViewCamera view;
	view.Position[0]     = header.CameraPosition.x.ToFloat();
	view.Position[1]     = header.CameraPosition.y.ToFloat();
	view.Position[2]     = header.CameraPosition.z.ToFloat();
	view.Rotation        = header.CameraRotation.ToFloat();
	view.FoVDeg          = header.CameraFoV.ToFloat();
	view.PrevPosition[0] = header.PrevCameraPosition.x.ToFloat();
	view.PrevPosition[1] = header.PrevCameraPosition.y.ToFloat();
	view.PrevPosition[2] = header.PrevCameraPosition.z.ToFloat();
	view.PrevRotation    = header.PrevCameraRotation.ToFloat();
	view.PrevFoVDeg      = header.PrevCameraFoV.ToFloat();
	return view;
}
