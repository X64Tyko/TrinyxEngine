#pragma once

#include <cstdint>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

#include "Jolt/Physics/Collision/ContactListener.h"

JPH_SUPPRESS_WARNINGS

// ---------------------------------------------------------------------------
// PhysicsContactEvent — pushed from Jolt worker threads (multi-producer) into
// JoltPhysics::ContactEventRing, drained by the Logic thread after
// PullActiveTransforms. OwnerWorld::OnContactEvent dispatches to listeners.
//
// BodyIDs are raw Jolt values. Use JoltPhysics::GetBodyOwner() to map a
// body back to an ECS entity index, or compare directly against a known BodyID
// (e.g. a TriggerVolume's sensor body that isn't an ECS entity).
// ---------------------------------------------------------------------------

enum class PhysicsContactEventType : uint8_t
{
	OnHit          = 0, // Two bodies first touched (or overlap started for sensors)
	Removed        = 1, // Two bodies separated
	OnOverlapBegin = 2,
	OnOverlapEnded = 3,
	Validate       = 4,
	Activated      = 5,
	Deactivated    = 6,
	Solve          = 7,
	Max            = 8,

	INVALID = 255
};

/// Simple manifold class, describes the contact surface between two bodies
class SimpleContactManifold
{
public:
	SimpleContactManifold() { WorldSpaceNormal = Vector3(), PenetrationDepth = 0.0f; }

	SimpleContactManifold(const JPH::ContactManifold& Manifold)
	{
		WorldSpaceNormal = Vector3(Manifold.mWorldSpaceNormal.GetX(), Manifold.mWorldSpaceNormal.GetY(), Manifold.mWorldSpaceNormal.GetZ());
		PenetrationDepth = Manifold.mPenetrationDepth;
	}

	/// Access to the world space contact positions
	Vector3 WorldSpaceNormal; ///< Normal for this manifold, direction along which to move body 2 out of collision along the shortest path
	float PenetrationDepth;   ///< Penetration depth (move shape 2 by this distance to resolve the collision). If this value is negative, this is a speculative contact point and may not actually result in a velocity change as during solving the bodies may not actually collide.
};

struct PhysicsContactEvent
{
	JPH::BodyID Body1;
	JPH::BodyID Body2;
	PhysicsContactEventType Type = PhysicsContactEventType::INVALID;
	SimpleContactManifold ContactInfo;
};
