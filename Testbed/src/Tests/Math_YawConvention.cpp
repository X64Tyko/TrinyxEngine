#include <cmath>

#include "TestFramework.h"
#include "QuatMath.h"

// One yaw convention for the whole engine: the basis a camera renders with (QuatFromYawPitch) must
// be the basis gameplay moves along. Positive yaw turns right, positive pitch looks up.
TEST(Math_YawPitchViewMatchesMoveBasis)
{
	constexpr float kTol  = 0.01f; // FastSin/FastCos precision, including Fixed32 builds
	const float yaws[]    = { 0.0f, 0.5f, 1.5707963f, -1.2f, 3.0f };
	const float pitches[] = { 0.0f, 0.4f, -0.7f };
	auto near             = [](float a, float b)
	{
		return std::fabs(a - b) < kTol;
	};

	for (float yaw : yaws)
	{
		for (float pitch : pitches)
		{
			const Quat q = QuatFromYawPitch(SimFloat(yaw), SimFloat(pitch));

			const Vector3 fwd = q.Rotate(Vector3{ SimFloat(0.0f), SimFloat(0.0f), SimFloat(-1.0f) });
			ASSERT(near(fwd.x.ToFloat(), std::sin(yaw) * std::cos(pitch)));
			ASSERT(near(fwd.y.ToFloat(), std::sin(pitch)));
			ASSERT(near(fwd.z.ToFloat(), -std::cos(yaw) * std::cos(pitch)));

			const Vector3 right = q.Rotate(Vector3{ SimFloat(1.0f), SimFloat(0.0f), SimFloat(0.0f) });
			ASSERT(near(right.x.ToFloat(), std::cos(yaw)));
			ASSERT(near(right.y.ToFloat(), 0.0f));
			ASSERT(near(right.z.ToFloat(), std::sin(yaw)));
		}
	}

	// Positive yaw is a right turn: a quarter turn from -Z faces +X.
	const Vector3 quarter = QuatFromYawPitch(SimFloat(1.5707963f), SimFloat(0.0f))
								.Rotate(Vector3{ SimFloat(0.0f), SimFloat(0.0f), SimFloat(-1.0f) });
	ASSERT(quarter.x.ToFloat() > 0.99f);
}
