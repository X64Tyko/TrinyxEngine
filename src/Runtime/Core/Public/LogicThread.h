#pragma once
#include "LogicThreadBase.h"
#include "Policies/FramePolicy.h"
#include "Policies/NetPolicy.h"
#include "Policies/RollbackPolicy.h"
#include "ConstructBatch.h"
#include "NetTypes.h"
#include "Registry.h"
#include "TemporalComponentCache.h"
#include "TrinyxJobs.h"
#include "TrinyxMPMCRing.h"
#include "TrinyxMPSCRing.h"
#include "Types.h"

template <typename, typename, typename>
class World;

// Headers needed by LogicThread.cpp method bodies (pulled in via this header
// since explicit instantiations in LogicThread.cpp include LogicThread.h):
#include "CameraManager.h"
#include "ConstructRegistry.h"
#include "EngineConfig.h"
#include "Input.h"
#include "JoltPhysics.h"
#include "Logger.h"
#include "Profiler.h"
#include "ThreadPinning.h"

// Net policies — included AFTER Registry.h to avoid incomplete-type errors in
// their inline template bodies (ReplicationSystem.h uses Registry internals).
// These headers define AuthoritySim, OwnerSim, SoloSim (via NetPolicy.h).
#include "AuthoritySim.h"
#include "OwnerSim.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>

/**
 * LogicThread<TNet, TRollback, TFrame> — The Brain.
 *
 * Three policy axes control behaviour:
 *   TNet      — OnSimInput / OnFramePublished dispatch (SoloSim / AuthoritySim / OwnerSim)
 *   TRollback — NoRollback (zero overhead) or RollbackSim (N-frame ring + correction queues)
 *   TFrame    — GameFrame (production) or EditorFrame (TickPause support)
 *
 * All method bodies live in LogicThread.cpp; explicit instantiations at the
 * bottom of that file compile all three needed specializations.
 */
template <typename TNet, typename TRollback, typename TFrame>
class LogicThread : public LogicThreadBase
{
public:
	LogicThread()           = default;
	~LogicThread() override = default;

	void Initialize(Registry* registry, const EngineConfig* config, JoltPhysics* physics,
					InputBuffer* simInput, InputBuffer* vizInput,
					TrinyxJobs::WorldQueueHandle worldQueue,
					const std::atomic<bool>* jobsInitialized,
					int windowWidth, int windowHeight) override;

	void Start() override;
	void Stop() override;
	void Join() override;

	/// Accessor used by AuthorityNet::WireNetMode / OwnerNet::WireNetMode to
	/// initialize the net policy after world creation.
	TNet& GetNetMode() { return NetMode; }

	uint32_t GetPhysicsDivizor() const { return PhysicsDivizor; }

private:
	friend struct RollbackSim;
	template <typename, typename, typename>
	friend class World;

	void ThreadMain();
	void ProcessVizInput(SimFloat dt);
	bool ProcessSimInput(SimFloat dt);
	void ScalarUpdate(SimFloat dt);
	void PrePhysics(SimFloat dt);
	void PostPhysics(SimFloat dt);

	void PublishCompletedFrame();
	void WaitForTiming(uint64_t frameStart, uint64_t perfFrequency);
	void TrackFPS();
	bool TickPause(uint64_t perfFrequency, uint64_t frameStartCounter, double dt);

	void PhysicsLoop(SimFloat fixedStepTime);
	bool FixedUpdate(uint64_t perfFrequency, SimFloat fixedStepTime, int maxPhysSubSteps,
					 uint64_t frameStartCounter);

	[[no_unique_address]] TNet NetMode;
	[[no_unique_address]] TRollback Rollback;
	[[no_unique_address]] TFrame FrameMode;
};

#ifdef TNX_ENABLE_ROLLBACK
#include "Policies/RollbackImpl.h"
#endif

// Explicit instantiations live in LogicThread.cpp. Suppress implicit
// instantiation in all other TUs so the LogicThread<> vtable has exactly one home.
extern template class LogicThread<SoloSim, NoRollback, GameFrame>;
extern template class LogicThread<AuthoritySim, NoRollback, GameFrame>;
extern template class LogicThread<OwnerSim, NoRollback, GameFrame>;
#ifdef TNX_ENABLE_ROLLBACK
extern template class LogicThread<SoloSim, RollbackSim, GameFrame>;
extern template class LogicThread<AuthoritySim, RollbackSim, GameFrame>;
extern template class LogicThread<OwnerSim, RollbackSim, GameFrame>;
#endif
