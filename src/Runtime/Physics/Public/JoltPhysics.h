#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Core/TempAllocator.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Events.h"
#include "PhysicsEvents.h"
#include "PhysicsTypes.h"
#include "Registry.h"
#include "TrinyxJobs.h"
#include "TrinyxMPSCRing.h"

JPH_SUPPRESS_WARNINGS

DEFINE_MULTICAST_CALLBACK(OnHitCB, PhysicsOnHitData)
DEFINE_MULTICAST_CALLBACK(OnOverlapBeginCB, PhysicsOverlapData)
DEFINE_MULTICAST_CALLBACK(OnOverlapEndCB, PhysicsOverlapData)

class JoltCharacter;
class JoltContactListener;
class JoltJobSystemAdapter;

namespace JPH
{
class BroadPhaseLayerInterfaceTable;
class ObjectVsBroadPhaseLayerFilterTable;
class ObjectLayerPairFilterTable;
} // namespace JPH
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

	// --- Body ownership registration ---
	void RegisterBody(JPH::BodyID id, EntityCacheHandle owner);
	void UnregisterBody(JPH::BodyID id);
	EntityCacheHandle GetBodyOwner(JPH::BodyID id) const;

	// --- JoltCharacter lifetime tracking ---
	void RegisterCharacter(JoltCharacter* ch);
	void UnregisterCharacter(JoltCharacter* ch);

	// --- Accessors ---

	JPH::PhysicsSystem* GetPhysicsSystem() { return PhysSystem.get(); }
	JPH::TempAllocator* GetTempAllocator() { return TempAllocator.get(); }
	JPH::BodyInterface& GetBodyInterface();
	const JPH::BodyInterface& GetBodyInterfaceNoLock() const;
	TrinyxJobs::JobCounter* GetJoltPhysCounter() { return &JoltPhysCounter; }
	uint32_t GetBodyCount() const { return PhysSystem->GetNumBodies(); }
	void ProcessContacts(const Registry* Reg);

	// --- Entity contact callbacks (keyed by EntityCacheHandle) ---

	template <typename T, void (T::*MemFn)(PhysicsOnHitData)>
	void BindOnHit(EntityHandle handle, Registry* reg, T* obj)
	{
		EntityCacheHandle idx = reg->GetRecord(handle).CacheEntityIndex;
		EnsureEntityCallbackSize(idx);
		OnHitCallbacks[idx].Bind<T, MemFn>(obj);
	}

	template <typename T, void (T::*MemFn)(PhysicsOverlapData)>
	void BindOnOverlapBegin(EntityHandle handle, Registry* reg, T* obj)
	{
		EntityCacheHandle idx = reg->GetRecord(handle).CacheEntityIndex;
		EnsureEntityCallbackSize(idx);
		OnOverlapBeginCallbacks[idx].Bind<T, MemFn>(obj);
	}

	template <typename T, void (T::*MemFn)(PhysicsOverlapData)>
	void BindOnOverlapEnd(EntityHandle handle, Registry* reg, T* obj)
	{
		EntityCacheHandle idx = reg->GetRecord(handle).CacheEntityIndex;
		EnsureEntityCallbackSize(idx);
		OnOverlapEndCallbacks[idx].Bind<T, MemFn>(obj);
	}

	void UnbindContacts(EntityHandle handle, Registry* reg, void* ctx);

	void UnbindContacts(EntityCacheHandle idx, void* ctx);

	// --- Construct contact callbacks (keyed by BodyID::GetIndex()) ---
	// For Construct-owned bodies with no ECS entity (e.g. JoltCharacter inner body).
	// representingEntity: passed as HitEntity to the other participant's callback.
	// JoltCharacter::Shutdown calls ClearConstructContacts — no destructor cleanup needed.

	template <typename T, void (T::*MemFn)(PhysicsOnHitData)>
	void BindConstructOnHit(JPH::BodyID id, T* obj, EntityHandle representingEntity = EntityHandle{})
	{
		uint32_t idx = id.GetIndex();
		EnsureConstructCallbackSize(idx);
		ConstructHitCallbacks[idx].Bind<T, MemFn>(obj);
		ConstructBodyOwnerPtrs[idx]     = static_cast<void*>(obj);
		ConstructBodyEntityHandles[idx] = representingEntity;
	}

	template <typename T, void (T::*MemFn)(PhysicsOverlapData)>
	void BindConstructOnOverlapBegin(JPH::BodyID id, T* obj, EntityHandle representingEntity = EntityHandle{})
	{
		uint32_t idx = id.GetIndex();
		EnsureConstructCallbackSize(idx);
		ConstructOverlapBeginCallbacks[idx].Bind<T, MemFn>(obj);
		ConstructBodyOwnerPtrs[idx]     = static_cast<void*>(obj);
		ConstructBodyEntityHandles[idx] = representingEntity;
	}

	template <typename T, void (T::*MemFn)(PhysicsOverlapData)>
	void BindConstructOnOverlapEnd(JPH::BodyID id, T* obj, EntityHandle representingEntity = EntityHandle{})
	{
		uint32_t idx = id.GetIndex();
		EnsureConstructCallbackSize(idx);
		ConstructOverlapEndCallbacks[idx].Bind<T, MemFn>(obj);
		ConstructBodyOwnerPtrs[idx]     = static_cast<void*>(obj);
		ConstructBodyEntityHandles[idx] = representingEntity;
	}

	void UnbindConstructContacts(JPH::BodyID id, void* ctx);

	void ClearConstructContacts(JPH::BodyID id);

	// --- Contact event ring ---
	// JoltContactListener pushes PhysicsContactEvents from Jolt worker threads
	std::optional<TrinyxMPSCRing<PhysicsContactEvent>::Consumer> ContactConsumer;

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
	bool GetIsActive() const { return bActive.load(std::memory_order_acquire); }

private:
	// Jolt subsystems (order matters for destruction)
	// Layer tables are per instance: every world (PIE runs several) owns the tables its
	// PhysicsSystem references. Declared before PhysSystem so they are destroyed after it.
	std::unique_ptr<JPH::BroadPhaseLayerInterfaceTable> BPLayerInterface;
	std::unique_ptr<JPH::ObjectLayerPairFilterTable> ObjPairFilter;
	std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> ObjVsBPFilter;
	std::unique_ptr<JoltJobSystemAdapter> JobSystem;
	std::unique_ptr<JPH::TempAllocatorImpl> TempAllocator;
	std::unique_ptr<JPH::PhysicsSystem> PhysSystem;
	std::unique_ptr<JoltContactListener> ContactListener;
	TrinyxMPSCRing<PhysicsContactEvent> ContactEventRing;
	std::atomic_bool bActive{ false };

	std::vector<JPH::BodyID> EntityToBody; // indexed by EntityCacheHandle
	std::vector<uint32_t> BodyToEntity;    // indexed by BodyID::GetIndex()

	// Entity contact callbacks — indexed by EntityCacheHandle.
	std::vector<OnHitCB> OnHitCallbacks;
	std::vector<OnOverlapBeginCB> OnOverlapBeginCallbacks;
	std::vector<OnOverlapEndCB> OnOverlapEndCallbacks;

	void EnsureEntityCallbackSize(EntityCacheHandle idx);

	// Construct contact callbacks — indexed by BodyID::GetIndex(), no BodyToEntity entry.
	std::vector<OnHitCB> ConstructHitCallbacks;
	std::vector<OnOverlapBeginCB> ConstructOverlapBeginCallbacks;
	std::vector<OnOverlapEndCB> ConstructOverlapEndCallbacks;
	std::vector<void*> ConstructBodyOwnerPtrs;
	std::vector<EntityHandle> ConstructBodyEntityHandles;

	void EnsureConstructCallbackSize(uint32_t idx);

	static constexpr uint32_t InvalidEntityIndex = UINT32_MAX;

	std::vector<JoltCharacter*> ActiveCharacters;

	const EngineConfig* ConfigPtr = nullptr;
	std::vector<uint64_t> LiveEntityBits; // Bitplane: 1 bit per entity index, reused across FlushPendingBodies calls
	alignas(FIELD_ARRAY_ALIGNMENT) std::vector<float> fieldScratch[7];
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
