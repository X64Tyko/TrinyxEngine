#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Core/TempAllocator.h>
#include <memory>
#include <vector>
#include <string>

#include "TrinyxJobs.h"

JPH_SUPPRESS_WARNINGS

class JoltJobSystemAdapter;
class Registry;
struct EngineConfig;

// JoltPhysics — engine-level wrapper around JPH::PhysicsSystem.
//
// Owns:
//   - PhysicsSystem + temp allocator + job adapter + layer config
//   - EntityIndex ↔ BodyID mapping arrays
//
// Lifetime: created by TrinyxEngine, passed to LogicThread.
// Step() is called once per fixed timestep from the Brain thread.
//
// Body lifecycle:
//   Entities with a JoltBody component are batch-registered via FlushPendingBodies().
//   After Step(), PullActiveTransforms() writes awake body transforms back to SoA.
//   Jolt owns the authoritative physics state — ECS only overrides on spawn/teleport/impulse.
class JoltPhysics
{
public:
	JoltPhysics();
	~JoltPhysics();

	JoltPhysics(const JoltPhysics&)            = delete;
	JoltPhysics& operator=(const JoltPhysics&) = delete;

	bool Initialize(const EngineConfig* config);
	void Shutdown();

	// Step the physics world by dt seconds. Called from Brain's fixed loop.
	void Step(float dt);

	// --- Body management ---

	// Scan entities with JoltBody components that have no Jolt body yet.
	// Creates bodies from component settings + initial transform, stores mapping.
	// Called once per physics tick (returns immediately if nothing pending).
	void FlushPendingBodies(Registry* reg);
	void PushKinematicTransforms(Registry* reg, float dt);

	// Write transforms from awake Jolt bodies back into SoA WriteArrays.
	// Called after Step(), before PostPhysics.
	void PullActiveTransforms(Registry* reg);

	// Remove a body when its entity is destroyed.
	void DestroyBody(uint32_t entityIndex);

	// Remove all bodies and clear all mapping arrays. Called during scene reset.
	void ResetAllBodies();

	// --- Mapping ---

	JPH::BodyID GetBodyID(uint32_t entityIndex) const;
	uint32_t GetEntityIndex(JPH::BodyID bodyID) const;
	bool HasBody(uint32_t entityIndex) const;

	// --- Accessors ---

	JPH::PhysicsSystem* GetPhysicsSystem() { return PhysSystem.get(); }
	JPH::TempAllocator* GetTempAllocator() { return TempAllocator.get(); }
	JPH::BodyInterface& GetBodyInterface();
	const JPH::BodyInterface& GetBodyInterfaceNoLock() const;
	TrinyxJobs::JobCounter* GetJoltPhysCounter() { return &JoltPhysCounter; }
	uint32_t GetBodyCount() const { return PhysSystem->GetNumBodies(); }

#ifdef TNX_ENABLE_ROLLBACK
	// --- State snapshot ring buffer ---
	// Save a full physics snapshot at a Flush+Pull boundary frame.
	void SaveSnapshot(uint32_t frameNumber);

	// Restore physics to the exact state saved at frameNumber.
	// Returns false if the requested frame is no longer in the ring buffer.
	bool RestoreSnapshot(uint32_t frameNumber);

	// Returns the oldest frame number in the snapshot ring, or UINT32_MAX if empty.
	// Used by LogicThread to clamp rollback targets to the range we can actually restore.
	uint32_t GetOldestSnapshotFrame() const;
#endif

private:
	// Jolt subsystems (order matters for destruction)
	std::unique_ptr<JoltJobSystemAdapter> JobSystem;
	std::unique_ptr<JPH::TempAllocatorImpl> TempAllocator;
	std::unique_ptr<JPH::PhysicsSystem> PhysSystem;

	// Entity ↔ Body mapping arrays.
	// EntityToBody: indexed by archetype global index → JPH::BodyID
	// BodyToEntity: indexed by BodyID.GetIndex() → archetype global index
	// Both default to invalid sentinel values (BodyID() / UINT32_MAX).
	std::vector<JPH::BodyID> EntityToBody;
	std::vector<uint32_t> BodyToEntity;

	static constexpr uint32_t InvalidEntityIndex = UINT32_MAX;

	const EngineConfig* ConfigPtr = nullptr;
	std::vector<uint64_t> LiveEntityBits; // Bitplane: 1 bit per entity index, reused across FlushPendingBodies calls
	alignas(16) std::vector<float> fieldScratch[7];
	TrinyxJobs::JobCounter JoltPhysCounter;

	struct SyncPair
	{
		JPH::BodyID id;
		uint32_t offset;
	};

	std::vector<SyncPair> syncList;

#ifdef TNX_ENABLE_ROLLBACK
	struct PhysicsSnapshot
	{
		uint32_t FrameNumber = UINT32_MAX;
		std::string Data;
	};
	std::vector<PhysicsSnapshot> SnapshotRing; // Sized at Initialize from TemporalFrameCount
	uint32_t SnapshotCapacity = 0;
#endif
};
