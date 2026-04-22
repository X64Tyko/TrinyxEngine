#pragma once

#include "ComponentView.h"
#include "SchemaReflector.h"
#include "CurveHandle.h"
#include "RegistryTypes.h"

// CameraLayer — blend/spring/FOV config for a single camera layer slot.
// OwnerHandle and TransitionCurveH are stored as UIntProxy (uint32_t values in chunk arrays).
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct CCameraLayer : ComponentView<CCameraLayer, WIDTH>
{
	TNX_REGISTER_FIELDS(CCameraLayer,
						BlendAlpha, TransitionSpeed,
						FOV, ArmLength,
		OffsetX, OffsetY, OffsetZ,
		SpringStiffness, SpringDamping,
		OwnerHandle, TransitionCurveH)

	FloatProxy<WIDTH> BlendAlpha{};      // target blend weight [0, 1]
	FloatProxy<WIDTH> TransitionSpeed{};

	FloatProxy<WIDTH> FOV{};       // degrees; 0 = slot default
	FloatProxy<WIDTH> ArmLength{}; // spring-arm length; 0 = no arm

	FloatProxy<WIDTH> OffsetX{}; // camera-space offset
	FloatProxy<WIDTH> OffsetY{};
	FloatProxy<WIDTH> OffsetZ{};

	FloatProxy<WIDTH> SpringStiffness{};
	FloatProxy<WIDTH> SpringDamping{};

	UIntProxy<WIDTH> OwnerHandle{};      // ConstructNetHandle::Value; 0 = unowned
	UIntProxy<WIDTH> TransitionCurveH{}; // CurveHandle::Value; 0 = linear

	CurveHandle GetTransitionCurve() const requires (WIDTH == FieldWidth::Scalar)
	{
		return CurveHandle{TransitionCurveH.Value()};
	}

	void SetTransitionCurve(CurveHandle h) requires (WIDTH == FieldWidth::Scalar)
	{
		TransitionCurveH = h.Value;
	}
};

TNX_REGISTER_COMPONENT(CCameraLayer)
