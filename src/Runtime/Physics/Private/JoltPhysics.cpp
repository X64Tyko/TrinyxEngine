#include "JoltPhysics.h"
#include "JoltJobSystemAdapter.h"
#include "EngineConfig.h"
#include "Logger.h"
#include "Profiler.h"
#include "TrinyxJobs.h"

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
#include "FieldMath.h"

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

// Find the field array table offset for a component by its TypeID.
// Returns -1 if the component is not in this archetype.
static int FindComponentFieldOffset(const Archetype* arch, ComponentTypeID typeID)
{
	const auto& layout = arch->CachedFieldArrayLayout;
	// Cache slot ID for the target component
	uint8_t targetSlot = ComponentFieldRegistry::Get().GetCacheSlotIndex(typeID);

	for (size_t i = 0; i < layout.size(); ++i)
	{
		if (layout[i].componentID == targetSlot) return static_cast<int>(i);
	}
	return -1;
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
	TempAllocator = std::make_unique<JPH::TempAllocatorImpl>(2048 * config->MAX_PHYSICS_ENTITIES);

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
	const JPH::uint cMaxBodies             = static_cast<JPH::uint>(config->MAX_PHYSICS_ENTITIES);
	const JPH::uint cNumBodyMutexes        = 0; // Jolt default
	const JPH::uint cMaxBodyPairs          = 65536;
	const JPH::uint cMaxContactConstraints = config->MAX_PHYSICS_ENTITIES * 2; // enough for everyone?

	PhysSystem = std::make_unique<JPH::PhysicsSystem>();
	PhysSystem->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
					 *s_BPLayerInterface, *s_ObjVsBPFilter, *s_ObjPairFilter);

	PhysSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

	// --- Entity ↔ Body mapping ---
	EntityToBody.resize(config->MAX_PHYSICS_ENTITIES, JPH::BodyID());
	BodyToEntity.resize(config->MAX_PHYSICS_ENTITIES, kInvalidEntityIndex);

	LOG_INFO_F("[JoltPhysics] Initialized — maxBodies=%u, tempAlloc=32MB", cMaxBodies);

	for (auto& vec : fieldScratch)
	{
		vec.reserve(config->MAX_PHYSICS_ENTITIES);
	}

	syncList.reserve(config->MAX_PHYSICS_ENTITIES);
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
	TNX_ZONE_NC("Jolt_Step", 0xFF4040FF);

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

void JoltPhysics::FlushPendingBodies(Registry* reg, uint32_t writeFrame, uint32_t volWriteFrame)
{
	TNX_ZONE_NC("Jolt_FlushBodies", 0xFF8040FF);

	// Query archetypes that contain JoltBody
	auto arches = reg->ComponentQuery<JoltBody<>>();
	if (arches.empty()) return;

	JPH::BodyInterface& bodyInterface = PhysSystem->GetBodyInterface();

	const ComponentTypeID bodyTypeID  = JoltBody<>::StaticTypeID();
	const ComponentTypeID transTypeID = TransRot<>::StaticTypeID();

	uint32_t bodiesCreated = 0;

	for (Archetype* arch : arches)
	{
		// Find field array table offsets for JoltBody and TransRot
		int bodyOffset  = FindComponentFieldOffset(arch, bodyTypeID);
		int transOffset = FindComponentFieldOffset(arch, transTypeID);
		if (bodyOffset < 0) continue; // shouldn't happen

		for (size_t chunkIdx = 0; chunkIdx < arch->Chunks.size(); ++chunkIdx)
		{
			Chunk* chunk         = arch->Chunks[chunkIdx];
			uint32_t entityCount = arch->GetChunkCount(chunkIdx);
			if (entityCount == 0) continue;

			void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
			arch->BuildFieldArrayTable(chunk, fieldArrayTable, writeFrame, volWriteFrame);

			// Bind JoltBody fields at their offset in the table
			JoltBody<> body;
			body.Bind(&fieldArrayTable[bodyOffset], nullptr);

			// Bind TransRot if present (for initial position/rotation)
			TransRot<> trans;
			bool hasTrans = (transOffset >= 0);
			if (hasTrans) trans.Bind(&fieldArrayTable[transOffset], nullptr);

			for (uint32_t i = 0; i < entityCount; ++i)
			{
				uint32_t globalIdx = chunk->Header.GlobalIndexStart + i;

				// Skip if body already exists for this entity
				if (globalIdx < EntityToBody.size() && !EntityToBody[globalIdx].IsInvalid())
				{
					body.Advance(1);
					if (hasTrans) trans.Advance(1);
					continue;
				}

				// Read body settings
				uint32_t shapeType = FieldMath::Read(body.Shape);
				float hx           = FieldMath::Read(body.HalfExtentX);
				float hy           = FieldMath::Read(body.HalfExtentY);
				float hz           = FieldMath::Read(body.HalfExtentZ);
				uint32_t motion    = FieldMath::Read(body.Motion);
				float mass         = FieldMath::Read(body.Mass);
				float friction     = FieldMath::Read(body.Friction);
				float restitution  = FieldMath::Read(body.Restitution);

				// Read initial transform (position + rotation)
				JPH::RVec3 pos(0, 0, 0);
				JPH::Quat rot = JPH::Quat::sIdentity();
				if (hasTrans)
				{
					pos = JPH::RVec3(
						FieldMath::Read(trans.PosX),
						FieldMath::Read(trans.PosY),
						FieldMath::Read(trans.PosZ));
					rot = JPH::Quat(
						FieldMath::Read(trans.RotQx),
						FieldMath::Read(trans.RotQy),
						FieldMath::Read(trans.RotQz),
						FieldMath::Read(trans.RotQw));
				}

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
					if (globalIdx >= EntityToBody.size()) EntityToBody.resize(globalIdx + 1024, JPH::BodyID());
					EntityToBody[globalIdx] = bodyID;

					uint32_t bodyIdx = bodyID.GetIndex();
					if (bodyIdx >= BodyToEntity.size()) BodyToEntity.resize(bodyIdx + 1024, kInvalidEntityIndex);
					BodyToEntity[bodyIdx] = globalIdx;

					bodiesCreated++;
				}

				body.Advance(1);
				if (hasTrans) trans.Advance(1);
			}
		}
	}

	if (bodiesCreated > 0)
	{
		LOG_INFO_F("[JoltPhysics] Created %u bodies", bodiesCreated);
	}
}

void JoltPhysics::PullActiveTransforms(Registry* reg)
{
	TNX_ZONE_NC("Jolt_PullTransforms", 0xFF8040FF);
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
