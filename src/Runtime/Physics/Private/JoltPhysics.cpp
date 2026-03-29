#include "JoltPhysics.h"
#include "JoltJobSystemAdapter.h"
#include "EngineConfig.h"
#include "Logger.h"
#include "Profiler.h"
#include "TrinyxJobs.h"

#include <cstdarg>

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>

#include "Registry.h"
#include "JoltBody.h"
#include "TransRot.h"

JPH_SUPPRESS_WARNINGS

// ---------------------------------------------------------------------------
// Jolt trace/assert callbacks
// ---------------------------------------------------------------------------

#ifdef JPH_ENABLE_ASSERTS
static void JoltTraceImpl(const char* inFMT, ...)
{
	char buffer[500]; // fits in LOG_DEBUG_F's 512-byte internal buffer with "[Jolt] " prefix
	va_list list;
	va_start(list, inFMT);
	vsnprintf(buffer, sizeof(buffer), inFMT, list);
	va_end(list);
	LOG_DEBUG_F("[Jolt] %s", buffer);
}

static bool JoltAssertFailedImpl(const char* inExpression, const char* inMessage,
								 const char* inFile, JPH::uint inLine)
{
	LOG_ERROR_F("[Jolt] ASSERT FAILED: %s:%u: (%s) %s",
				inFile, inLine, inExpression, inMessage ? inMessage : "");
	return true; // trigger debugger breakpoint
}
#endif

// ---------------------------------------------------------------------------
// Layer configuration
// ---------------------------------------------------------------------------

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

// Using the Table implementations — no virtual overrides needed, just a lookup table.
// These must outlive the PhysicsSystem, so they're file-static singletons initialized in Initialize().
static JPH::BroadPhaseLayerInterfaceTable* s_BPLayerInterface   = nullptr;
static JPH::ObjectVsBroadPhaseLayerFilterTable* s_ObjVsBPFilter = nullptr;
static JPH::ObjectLayerPairFilterTable* s_ObjPairFilter         = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Map JoltMotion:: constants to JPH::EMotionType
static JPH::EMotionType ToJoltMotionType(uint32_t motion)
{
	switch (motion)
	{
		case 0: return JPH::EMotionType::Static;
		case 1: return JPH::EMotionType::Kinematic;
		default: return JPH::EMotionType::Dynamic;
	}
}

// Map JoltMotion:: constants to the appropriate object layer
static JPH::ObjectLayer ToJoltLayer(uint32_t motion)
{
	return (motion == 0) ? JoltLayers::Static : JoltLayers::Dynamic;
}

// Create a Jolt shape from JoltBody component settings (single entity, scalar access).
// Returns a ref-counted shape pointer. Jolt handles deduplication internally.
static JPH::RefConst<JPH::Shape> CreateShapeFromSettings(
	uint32_t shapeType, float hx, float hy, float hz)
{
	switch (shapeType)
	{
		case 1: // Sphere
			return new JPH::SphereShape(hx);
		case 2: // Capsule
			return new JPH::CapsuleShape(hy, hx);
		default: // Box (0 or unknown)
			return new JPH::BoxShape(JPH::Vec3(hx, hy, hz));
	}
}

// ---------------------------------------------------------------------------
// JoltPhysics implementation
// ---------------------------------------------------------------------------

JoltPhysics::JoltPhysics() = default;

JoltPhysics::~JoltPhysics()
{
	Shutdown();
}

bool JoltPhysics::Initialize(const EngineConfig* config)
{
	TNX_ZONE_N("JoltPhysics_Init");
	ConfigPtr = config;

	// --- Jolt global init (idempotent) ---
	JPH::RegisterDefaultAllocator();
	JPH_IF_ENABLE_ASSERTS(JPH::Trace = JoltTraceImpl;)
	JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = JoltAssertFailedImpl;)

	JPH::Factory::sInstance = new JPH::Factory();
	JPH::RegisterTypes();

	// --- Temp allocator: 32 MB pre-allocated scratch space ---
	TempAllocator = std::make_unique<JPH::TempAllocatorImpl>(2048 * config->MAX_JOLT_BODIES);

	// --- Job system adapter ---
	constexpr JPH::uint kMaxJobs     = JPH::cMaxPhysicsJobs;
	constexpr JPH::uint kMaxBarriers = JPH::cMaxPhysicsBarriers;
	JobSystem                        = std::make_unique<JoltJobSystemAdapter>(kMaxJobs, kMaxBarriers, &JoltPhysCounter);

	// --- Layer configuration (table-based, no virtual overrides) ---
	s_BPLayerInterface = new JPH::BroadPhaseLayerInterfaceTable(
		JoltLayers::NumLayers, JoltBroadPhaseLayers::NumLayers);
	s_BPLayerInterface->MapObjectToBroadPhaseLayer(JoltLayers::Static, JoltBroadPhaseLayers::Static);
	s_BPLayerInterface->MapObjectToBroadPhaseLayer(JoltLayers::Dynamic, JoltBroadPhaseLayers::Dynamic);

	s_ObjVsBPFilter = new JPH::ObjectVsBroadPhaseLayerFilterTable(
		*s_BPLayerInterface, JoltBroadPhaseLayers::NumLayers,
		*new JPH::ObjectLayerPairFilterTable(JoltLayers::NumLayers), // temporary for construction
		JoltLayers::NumLayers);

	s_ObjPairFilter = new JPH::ObjectLayerPairFilterTable(JoltLayers::NumLayers);
	s_ObjPairFilter->EnableCollision(JoltLayers::Static, JoltLayers::Dynamic);
	s_ObjPairFilter->EnableCollision(JoltLayers::Dynamic, JoltLayers::Dynamic);

	// Rebuild the broadphase filter with the real pair filter
	delete s_ObjVsBPFilter;
	s_ObjVsBPFilter = new JPH::ObjectVsBroadPhaseLayerFilterTable(
		*s_BPLayerInterface, JoltBroadPhaseLayers::NumLayers,
		*s_ObjPairFilter, JoltLayers::NumLayers);

	// --- Physics system ---
	const JPH::uint cMaxBodies             = static_cast<JPH::uint>(config->MAX_JOLT_BODIES);
	const JPH::uint cNumBodyMutexes        = 0; // Jolt default
	const JPH::uint cMaxBodyPairs          = 65536;
	const JPH::uint cMaxContactConstraints = config->MAX_JOLT_BODIES * 2; // enough for everyone?

	PhysSystem = std::make_unique<JPH::PhysicsSystem>();
	PhysSystem->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
					 *s_BPLayerInterface, *s_ObjVsBPFilter, *s_ObjPairFilter);

	PhysSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

	// --- Entity ↔ Body mapping ---
	EntityToBody.resize(config->MAX_JOLT_BODIES, JPH::BodyID());
	BodyToEntity.resize(config->MAX_JOLT_BODIES, kInvalidEntityIndex);

	LOG_INFO_F("[JoltPhysics] Initialized — maxBodies=%u, tempAlloc=%uMB, maxConcurrency=%d",
			   cMaxBodies, (2048 * config->MAX_JOLT_BODIES) / (1024 * 1024),
			   JobSystem->GetMaxConcurrency());

	for (auto& vec : fieldScratch)
	{
		vec.reserve(config->MAX_JOLT_BODIES);
	}

	syncList.reserve(config->MAX_JOLT_BODIES);
	return true;
}

void JoltPhysics::Shutdown()
{
	if (!PhysSystem) return;

	PhysSystem.reset();
	JobSystem.reset();
	TempAllocator.reset();

	JPH::UnregisterTypes();

	delete JPH::Factory::sInstance;
	JPH::Factory::sInstance = nullptr;

	delete s_ObjVsBPFilter;
	s_ObjVsBPFilter = nullptr;
	delete s_ObjPairFilter;
	s_ObjPairFilter = nullptr;
	delete s_BPLayerInterface;
	s_BPLayerInterface = nullptr;

	LOG_INFO("[JoltPhysics] Shutdown complete");
}

void JoltPhysics::Step(float dt)
{
	TNX_ZONE_NC("Jolt_Step", TNX_COLOR_JOLT);

	// Jolt recommends 1 collision step per 1/60s. At 128Hz that's ~2 steps,
	// but we're already substepping in the engine's accumulator loop, so
	// each call here is exactly one fixed step.
	//constexpr int cCollisionSteps = 1; // Leaving this at 1 even though the default fixed steps per physics is 8 so that physics is running at 64Hz

	// manual calculation based on update rates.
	static int cCollisionSteps = std::max(1, ConfigPtr->FixedUpdateHz / ConfigPtr->PhysicsUpdateInterval / 64);

	PhysSystem->Update(dt, cCollisionSteps, TempAllocator.get(), JobSystem.get());
}

// ---------------------------------------------------------------------------
// Body management
// ---------------------------------------------------------------------------

void JoltPhysics::FlushPendingBodies(Registry* reg)
{
	TNX_ZONE_NC("Jolt_FlushBodies", TNX_COLOR_JOLT);

	// JoltBody is volatile, TransRot is temporal — they may live in different slabs
	// when TNX_ENABLE_ROLLBACK is on. Get the correct cache for each component.
	ComponentCacheBase* VC = reg->GetVolatileCache(); // JoltBody lives here
	ComponentCacheBase* TC = reg->GetTemporalCache(); // TransRot lives here

	// Physics range is identical across both caches (allocators advance in sync).
	auto [physStart, physEnd] = VC->GetPhysicsRange();
	if (physStart >= physEnd) return; // No physics entities allocated

	JPH::BodyInterface& bodyInterface = PhysSystem->GetBodyInterface();

	// Get slab field arrays for the active write frame (direct slab access).
	// JoltBody fields from VolatileCache, TransRot fields from TemporalCache.
	TemporalFrameHeader* volHeader = VC->GetFrameHeader();
	TemporalFrameHeader* tmpHeader = TC->GetFrameHeader();

	const uint8_t bodySlot = JoltBody<>::StaticTemporalIndex();
	auto* slabShape        = static_cast<uint32_t*>(VC->GetFieldData(volHeader, bodySlot, 0));
	auto* slabHalfX        = static_cast<float*>(VC->GetFieldData(volHeader, bodySlot, 1));
	auto* slabHalfY        = static_cast<float*>(VC->GetFieldData(volHeader, bodySlot, 2));
	auto* slabHalfZ        = static_cast<float*>(VC->GetFieldData(volHeader, bodySlot, 3));
	auto* slabMotion       = static_cast<uint32_t*>(VC->GetFieldData(volHeader, bodySlot, 4));
	auto* slabMass         = static_cast<float*>(VC->GetFieldData(volHeader, bodySlot, 5));
	auto* slabFriction     = static_cast<float*>(VC->GetFieldData(volHeader, bodySlot, 6));
	auto* slabRestit       = static_cast<float*>(VC->GetFieldData(volHeader, bodySlot, 7));

	const uint8_t transSlot = TransRot<>::StaticTemporalIndex();
	auto* slabPosX          = static_cast<float*>(TC->GetFieldData(tmpHeader, transSlot, 0));
	auto* slabPosY          = static_cast<float*>(TC->GetFieldData(tmpHeader, transSlot, 1));
	auto* slabPosZ          = static_cast<float*>(TC->GetFieldData(tmpHeader, transSlot, 2));
	auto* slabRotX          = static_cast<float*>(TC->GetFieldData(tmpHeader, transSlot, 3));
	auto* slabRotY          = static_cast<float*>(TC->GetFieldData(tmpHeader, transSlot, 4));
	auto* slabRotZ          = static_cast<float*>(TC->GetFieldData(tmpHeader, transSlot, 5));
	auto* slabRotW          = static_cast<float*>(TC->GetFieldData(tmpHeader, transSlot, 6));

	if (!slabShape || !slabPosX) return; // Fields not allocated yet

	// Bitplane tracking which entity indices are alive (1 bit per entity).
	// Sized to cover the full physics range even if EntityToBody hasn't grown that far.
	const size_t maxIdx        = std::max(static_cast<size_t>(physEnd), EntityToBody.size());
	const size_t bitplaneWords = (maxIdx + 63) / 64;
	if (LiveEntityBits.size() < bitplaneWords) LiveEntityBits.resize(bitplaneWords);
	std::fill(LiveEntityBits.begin(), LiveEntityBits.begin() + bitplaneWords, 0ULL);

	uint32_t bodiesCreated = 0;

	// Iterate the contiguous DUAL+PHYS slab region.
	// Fast path (body exists): single array check — no archetype/chunk indirection.
	// Slow path (new entity): read all fields directly from slab by cache index.
	for (uint32_t idx = physStart; idx < physEnd; ++idx)
	{
		// Mark this cache index as alive in the bitplane
		LiveEntityBits[idx / 64] |= (1ULL << (idx % 64));

		// Fast path: body already exists for this entity
		if (idx < EntityToBody.size() && !EntityToBody[idx].IsInvalid()) continue;

		// Skip uninitialized slots — zero half-extents means no JoltBody data was written.
		// Can't use Shape/Motion as sentinels since Box=0 and Static=0 are valid values.
		if (slabHalfX[idx] == 0.0f && slabHalfY[idx] == 0.0f && slabHalfZ[idx] == 0.0f) continue;

		uint32_t shapeType = slabShape[idx];

		// Read JoltBody fields directly from slab
		float hx          = slabHalfX[idx];
		float hy          = slabHalfY[idx];
		float hz          = slabHalfZ[idx];
		uint32_t motion   = slabMotion[idx];
		float mass        = slabMass[idx];
		float friction    = slabFriction[idx];
		float restitution = slabRestit[idx];

		// Read initial transform from slab
		JPH::RVec3 pos(slabPosX[idx], slabPosY[idx], slabPosZ[idx]);
		JPH::Quat rot(slabRotX[idx], slabRotY[idx], slabRotZ[idx], slabRotW[idx]);

		// Guard against zero/denorm quaternions (zero-initialized fields
		// or imprecise scene file values). Jolt asserts on unnormalized quats.
		if (rot.LengthSq() < 1.0e-6f) rot = JPH::Quat::sIdentity();
		else rot                          = rot.Normalized();

		// Create shape
		JPH::RefConst<JPH::Shape> shape = CreateShapeFromSettings(shapeType, hx, hy, hz);

		// Build body creation settings
		JPH::BodyCreationSettings settings(
			shape, pos, rot,
			ToJoltMotionType(motion),
			ToJoltLayer(motion));

		settings.mFriction    = friction;
		settings.mRestitution = restitution;
		if (motion == 2) // Dynamic
		{
			settings.mOverrideMassProperties       = JPH::EOverrideMassProperties::CalculateInertia;
			settings.mMassPropertiesOverride.mMass = mass;
		}

		// Create and add body
		JPH::Body* joltBody = bodyInterface.CreateBody(settings);
		if (joltBody)
		{
			JPH::BodyID bodyID = joltBody->GetID();
			bodyInterface.AddBody(bodyID, JPH::EActivation::Activate);

			// Store mapping
			if (idx >= EntityToBody.size()) EntityToBody.resize(idx + 1024, JPH::BodyID());
			EntityToBody[idx] = bodyID;

			uint32_t bodyIdx = bodyID.GetIndex();
			if (bodyIdx >= BodyToEntity.size()) BodyToEntity.resize(bodyIdx + 1024, kInvalidEntityIndex);
			BodyToEntity[bodyIdx] = idx;

			bodiesCreated++;
		}
	}

	// Destroy orphaned Jolt bodies — any mapped body whose entity isn't in the physics range.
	// Entities outside [physStart, physEnd) have unset bits, so their bodies get cleaned up.
	uint32_t bodiesDestroyed = 0;
	for (size_t word = 0; word < bitplaneWords; ++word)
	{
		// Invert: bits that are 0 in liveBits are candidates for orphan cleanup.
		// Scan 64 entities at a time — skip entire words where all entities are alive.
		uint64_t deadMask = ~LiveEntityBits[word];
		while (deadMask)
		{
			uint32_t bit = TNX_CTZ64(deadMask);
			uint32_t idx = static_cast<uint32_t>(word * 64 + bit);
			deadMask     &= deadMask - 1; // clear lowest set bit

			if (idx >= EntityToBody.size()) break;
			if (EntityToBody[idx].IsInvalid()) continue;

			bodyInterface.RemoveBody(EntityToBody[idx]);
			bodyInterface.DestroyBody(EntityToBody[idx]);

			uint32_t bodyIdx = EntityToBody[idx].GetIndex();
			if (bodyIdx < BodyToEntity.size()) BodyToEntity[bodyIdx] = kInvalidEntityIndex;
			EntityToBody[idx] = JPH::BodyID();
			bodiesDestroyed++;
		}
	}

	if (bodiesCreated > 0 || bodiesDestroyed > 0)
	{
		LOG_INFO_F("[JoltPhysics] Created %u bodies, destroyed %u orphans", bodiesCreated, bodiesDestroyed);
	}
}

void JoltPhysics::PullActiveTransforms(Registry* reg)
{
	TNX_ZONE_NC("Jolt_PullTransforms", TNX_COLOR_JOLT);
	TrinyxJobs::WaitForCounter(&JoltPhysCounter, TrinyxJobs::Queue::Physics);

	JPH::BodyIDVector activeIDs;
	PhysSystem->GetActiveBodies(JPH::EBodyType::RigidBody, activeIDs);
	const int32_t activeCount                         = activeIDs.size();
	const JPH::BodyLockInterfaceNoLock& bodyInterface = PhysSystem->GetBodyLockInterfaceNoLock();

	syncList.resize(activeCount);

	for (int32_t i = 0; i < activeCount; ++i)
	{
		syncList[i] = {activeIDs[i], BodyToEntity[activeIDs[i].GetIndex()]};
	}

	// Sort by offset for strictly ascending memory writes later!
	std::sort(syncList.begin(), syncList.end(), [](const SyncPair& a, const SyncPair& b)
	{
		return a.offset < b.offset;
	});

	int32_t i = 0;
	for (; i < (activeCount - 4); i += 4)
	{
		JPH::BodyLockRead lock0(bodyInterface, syncList[i].id);
		JPH::BodyLockRead lock1(bodyInterface, syncList[i + 1].id);
		JPH::BodyLockRead lock2(bodyInterface, syncList[i + 2].id);
		JPH::BodyLockRead lock3(bodyInterface, syncList[i + 3].id);

		JPH::Vec4 pos0(lock0.GetBody().GetPosition());
		JPH::Vec4 pos1(lock1.GetBody().GetPosition());
		JPH::Vec4 pos2(lock2.GetBody().GetPosition());
		JPH::Vec4 pos3(lock3.GetBody().GetPosition());

		// 4. TRANSPOSE!
		_MM_TRANSPOSE4_PS(pos0.mValue, pos1.mValue, pos2.mValue, pos3.mValue);

		_mm_stream_ps(fieldScratch[0].data() + i, pos0.mValue);
		_mm_stream_ps(fieldScratch[1].data() + i, pos1.mValue);
		_mm_stream_ps(fieldScratch[2].data() + i, pos2.mValue);

		pos0 = lock0.GetBody().GetRotation().GetXYZW();
		pos1 = lock1.GetBody().GetRotation().GetXYZW();
		pos2 = lock2.GetBody().GetRotation().GetXYZW();
		pos3 = lock3.GetBody().GetRotation().GetXYZW();

		// 4. TRANSPOSE!
		_MM_TRANSPOSE4_PS(pos0.mValue, pos1.mValue, pos2.mValue, pos3.mValue);

		_mm_stream_ps(fieldScratch[3].data() + i, pos0.mValue);
		_mm_stream_ps(fieldScratch[4].data() + i, pos1.mValue);
		_mm_stream_ps(fieldScratch[5].data() + i, pos2.mValue);
		_mm_stream_ps(fieldScratch[6].data() + i, pos3.mValue);
	}

	for (; i < activeCount; ++i)
	{
		JPH::BodyLockRead lock(bodyInterface, syncList[i].id);
		JPH::Vec4 pos(lock.GetBody().GetPosition());
		JPH::Vec4 rot	(lock.GetBody().GetRotation().GetXYZW());
		fieldScratch[0].data()[i] = pos[0];
		fieldScratch[1].data()[i] = pos[1];
		fieldScratch[2].data()[i] = pos[2];
		fieldScratch[3].data()[i] = rot[0];
		fieldScratch[4].data()[i] = rot[1];
		fieldScratch[5].data()[i] = rot[2];
		fieldScratch[6].data()[i] = rot[3];
	}
	
	_mm_sfence();

	ComponentCacheBase* TC = reg->GetTemporalCache();

	uint32_t writeFrame = 0;
	while (!TC->TryLockFrameForWrite(writeFrame)) { _mm_pause(); }

	TrinyxJobs::JobCounter writebackCounter;

	for (i = 0; i < 7; i++)
	{
		float* fieldPtr = fieldScratch[i].data();
		TrinyxJobs::Dispatch([this, TC, i, fieldPtr](uint32_t)
		{
			float* fieldArr = static_cast<float*>(TC->GetFieldData(TC->GetFrameHeader(), TransRot<>::StaticTemporalIndex(), i));
			int idx         = 0;
			for (auto& Entity : syncList)
			{
				float* field = fieldArr + Entity.offset;
				*field       = fieldPtr[idx++];
			}
		}, &writebackCounter, TrinyxJobs::Queue::Logic);
	}

	// Because we're waiting for jobs to finish within the func we shouldn't need to worry about scope loss
	TrinyxJobs::WaitForCounter(&writebackCounter, TrinyxJobs::Queue::Logic);
	TC->UnlockFrameWrite();
}

void JoltPhysics::DestroyBody(uint32_t entityIndex)
{
	if (entityIndex >= EntityToBody.size()) return;

	JPH::BodyID bodyID = EntityToBody[entityIndex];
	if (bodyID.IsInvalid()) return;

	JPH::BodyInterface& bodyInterface = PhysSystem->GetBodyInterface();
	bodyInterface.RemoveBody(bodyID);
	bodyInterface.DestroyBody(bodyID);

	// Clear mapping
	uint32_t bodyIdx = bodyID.GetIndex();
	if (bodyIdx < BodyToEntity.size()) BodyToEntity[bodyIdx] = kInvalidEntityIndex;
	EntityToBody[entityIndex] = JPH::BodyID();
}

void JoltPhysics::ResetAllBodies()
{
	TNX_ZONE_NC("Jolt_ResetAllBodies", TNX_COLOR_JOLT);

	JPH::BodyInterface& bodyInterface = PhysSystem->GetBodyInterface();

	uint32_t bodiesDestroyed = 0;
	for (uint32_t idx = 0; idx < EntityToBody.size(); ++idx)
	{
		if (EntityToBody[idx].IsInvalid()) continue;

		bodyInterface.RemoveBody(EntityToBody[idx]);
		bodyInterface.DestroyBody(EntityToBody[idx]);
		bodiesDestroyed++;
	}

	std::fill(EntityToBody.begin(), EntityToBody.end(), JPH::BodyID());
	std::fill(BodyToEntity.begin(), BodyToEntity.end(), kInvalidEntityIndex);

	if (bodiesDestroyed > 0)
	{
		LOG_INFO_F("[JoltPhysics] Reset: destroyed %u bodies", bodiesDestroyed);
	}
}

// ---------------------------------------------------------------------------
// Mapping accessors
// ---------------------------------------------------------------------------

JPH::BodyID JoltPhysics::GetBodyID(uint32_t entityIndex) const
{
	if (entityIndex >= EntityToBody.size()) return JPH::BodyID();
	return EntityToBody[entityIndex];
}

uint32_t JoltPhysics::GetEntityIndex(JPH::BodyID bodyID) const
{
	uint32_t idx = bodyID.GetIndex();
	if (idx >= BodyToEntity.size()) return kInvalidEntityIndex;
	return BodyToEntity[idx];
}

bool JoltPhysics::HasBody(uint32_t entityIndex) const
{
	return entityIndex < EntityToBody.size() && !EntityToBody[entityIndex].IsInvalid();
}

JPH::BodyInterface& JoltPhysics::GetBodyInterface()
{
	return PhysSystem->GetBodyInterface();
}

const JPH::BodyInterface& JoltPhysics::GetBodyInterfaceNoLock() const
{
	return PhysSystem->GetBodyInterfaceNoLock();
}
