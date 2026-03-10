#include "JoltPhysics.h"
#include "JoltJobSystemAdapter.h"
#include "EngineConfig.h"
#include "Logger.h"
#include "Profiler.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>

JPH_SUPPRESS_WARNINGS

// ---------------------------------------------------------------------------
// Jolt trace/assert callbacks
// ---------------------------------------------------------------------------

static void JoltTraceImpl(const char* inFMT, ...)
{
	char buffer[500]; // fits in LOG_DEBUG_F's 512-byte internal buffer with "[Jolt] " prefix
	va_list list;
	va_start(list, inFMT);
	vsnprintf(buffer, sizeof(buffer), inFMT, list);
	va_end(list);
	LOG_DEBUG_F("[Jolt] %s", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
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
	JPH::Trace = JoltTraceImpl;
	JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = JoltAssertFailedImpl;
	)

	JPH::Factory::sInstance = new JPH::Factory();
	JPH::RegisterTypes();

	// --- Temp allocator: 16 MB pre-allocated scratch space ---
	TempAllocator = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);

	// --- Job system adapter ---
	constexpr JPH::uint kMaxJobs     = JPH::cMaxPhysicsJobs;
	constexpr JPH::uint kMaxBarriers = JPH::cMaxPhysicsBarriers;
	JobSystem                        = std::make_unique<JoltJobSystemAdapter>(kMaxJobs, kMaxBarriers);

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
	const JPH::uint cMaxBodies             = static_cast<JPH::uint>(config->MAX_CACHED_ENTITIES);
	const JPH::uint cNumBodyMutexes        = 0; // Jolt default
	const JPH::uint cMaxBodyPairs          = 65536;
	const JPH::uint cMaxContactConstraints = 10240;

	PhysSystem = std::make_unique<JPH::PhysicsSystem>();
	PhysSystem->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
					 *s_BPLayerInterface, *s_ObjVsBPFilter, *s_ObjPairFilter);

	PhysSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

	// --- Entity ↔ Body mapping ---
	EntityToBody.resize(config->MAX_CACHED_ENTITIES, JPH::BodyID());
	BodyToEntity.resize(config->MAX_CACHED_ENTITIES, kInvalidEntityIndex);

	LOG_INFO_F("[JoltPhysics] Initialized — maxBodies=%u, tempAlloc=16MB", cMaxBodies);
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
	constexpr int cCollisionSteps = 1;

	PhysSystem->Update(dt, cCollisionSteps, TempAllocator.get(), JobSystem.get());
}