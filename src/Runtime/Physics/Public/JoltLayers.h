#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>

namespace JoltLayers
{
	static constexpr JPH::ObjectLayer Static  = 0;
	static constexpr JPH::ObjectLayer Dynamic = 1;
	static constexpr JPH::uint NumLayers      = 2;
}

namespace JoltBroadPhaseLayers
{
	static constexpr JPH::BroadPhaseLayer Static(0);
	static constexpr JPH::BroadPhaseLayer Dynamic(1);
	static constexpr JPH::uint NumLayers = 2;
}