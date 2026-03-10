#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Core/TempAllocator.h>
#include <memory>
#include <vector>

JPH_SUPPRESS_WARNINGS

class JoltJobSystemAdapter;
struct EngineConfig;

// JoltPhysics — engine-level wrapper around JPH::PhysicsSystem.
//
// Owns:
//   - PhysicsSystem + temp allocator + job adapter + layer config
//   - EntityIndex ↔ BodyID mapping arrays
//
// Lifetime: created by TrinyxEngine, passed to LogicThread.
// Step() is called once per fixed timestep from the Brain thread.
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

	// --- Body management (future — not yet wired) ---

	// Create a Jolt body for an entity. Returns the BodyID.
	// JPH::BodyID CreateBody(uint32_t entityIndex, const JPH::BodyCreationSettings& settings);
	// void DestroyBody(uint32_t entityIndex);

	// --- Mapping (future — not yet wired) ---

	// JPH::BodyID GetBodyID(uint32_t entityIndex) const;
	// uint32_t    GetEntityIndex(JPH::BodyID bodyID) const;

	// --- Accessors ---
	JPH::PhysicsSystem* GetPhysicsSystem() { return PhysSystem.get(); }

private:
	// Jolt subsystems (order matters for destruction)
	std::unique_ptr<JoltJobSystemAdapter> JobSystem;
	std::unique_ptr<JPH::TempAllocatorImpl> TempAllocator;
	std::unique_ptr<JPH::PhysicsSystem> PhysSystem;

	// Entity ↔ Body mapping arrays.
	// EntityToBody: indexed by entity global index → JPH::BodyID
	// BodyToEntity: indexed by BodyID.GetIndex() (lower 23 bits) → entity global index
	// Both default to invalid sentinel values (BodyID() / UINT32_MAX).
	std::vector<JPH::BodyID> EntityToBody;
	std::vector<uint32_t> BodyToEntity;

	static constexpr uint32_t kInvalidEntityIndex = UINT32_MAX;

	const EngineConfig* ConfigPtr = nullptr;
};