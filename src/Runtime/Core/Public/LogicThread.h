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

/** @addtogroup core
 *  @{
 */

template <typename, typename, typename>
class World;

// Headers needed by LogicThread.cpp explicit-instantiation TU — pulled in here
// because LogicThread.cpp includes LogicThread.h to instantiate all specializations.
#include "CameraManager.h"
#include "ConstructRegistry.h"
#include "EngineConfig.h"
#include "Input.h"
#include "JoltPhysics.h"
#include "Logger.h"
#include "Profiler.h"
#include "ThreadPinning.h"

// Net policies included after Registry.h — their template bodies reference
// ReplicationSystem, which in turn uses Registry internals.
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
 * @brief The Brain — fixed-timestep simulation coordinator for one World.
 *
 * Three independent compile-time policy axes produce exactly one concrete type:
 *
 * @tparam TNet      Input injection and replication dispatch.
 *                   `SoloSim` (offline) | `AuthoritySim` (server/Host) | `OwnerSim` (client).
 * @tparam TRollback Correction history.
 *                   `NoRollback` (zero overhead) | `RollbackSim` (N-frame ring + queues).
 * @tparam TFrame    Editor branching.
 *                   `GameFrame` (shipping) | `EditorFrame` (PIE / tick-pause).
 *
 * All method bodies live in `LogicThread.cpp`; explicit instantiations there are
 * the only definitions. External code holds `LogicThreadBase*` to avoid coupling
 * to a specific specialization.
 */
template <typename TNet, typename TRollback, typename TFrame>
class LogicThread : public LogicThreadBase
{
public:
	LogicThread()           = default;
	~LogicThread() override = default;

	/**
	 * @brief Bind all non-owning pointers and prepare the thread for Start().
	 * @param worldQueue  World-scoped job queue handle; Brain submits pre/post-physics jobs here.
	 * @param jobsInitialized Engine jobs-ready gate; Brain spins on this before draining the queue.
	 */
	void Initialize(Registry* registry, const EngineConfig* config, JoltPhysics* physics,
		InputBuffer* simInput, InputBuffer* vizInput,
		TrinyxJobs::WorldQueueHandle worldQueue,
		const std::atomic<bool>* jobsInitialized,
		int windowWidth, int windowHeight) override;

	void Start() override; ///< @brief Spawn the Brain thread and begin the fixed-rate loop.
	void Stop() override;  ///< @brief Signal the Brain thread to exit after the current frame.
	void Join() override;  ///< @brief Block the caller until the Brain thread exits.

	/// @brief Expose the concrete net policy so AuthorityNet/OwnerNet can call Bind() after world creation.
	TNet& GetNetMode() { return NetMode; }

	/// @brief Physics sub-step divisor derived from EngineConfig at Initialize time.
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
	void TickOnce();
	bool TickPause(uint64_t perfFrequency, uint64_t frameStartCounter, double dt);

	void PhysicsLoop(SimFloat fixedStepTime);
	bool FixedUpdate(uint64_t perfFrequency, SimFloat fixedStepTime, int maxPhysSubSteps,
		uint64_t frameStartCounter);

	[[no_unique_address]] TNet NetMode;       ///< Net policy — zero size for SoloSim.
	[[no_unique_address]] TRollback Rollback; ///< Rollback policy — zero size for NoRollback.
	[[no_unique_address]] TFrame FrameMode;   ///< Frame policy — zero size (tag type only).
};

// Explicit instantiations live in LogicThread.cpp. Suppress implicit instantiation
// in all other TUs so LogicThread<>'s vtable has exactly one home.
// AuthoritySim/OwnerSim variants depend on Net/Private symbols and only exist
// when networking is enabled.
extern template class LogicThread<SoloSim, NoRollback, GameFrame>;
#ifdef TNX_ENABLE_NETWORK
extern template class LogicThread<AuthoritySim, NoRollback, GameFrame>;
extern template class LogicThread<OwnerSim, NoRollback, GameFrame>;
#endif
#ifdef TNX_ENABLE_ROLLBACK
extern template class LogicThread<SoloSim, RollbackSim, GameFrame>;
#ifdef TNX_ENABLE_NETWORK
extern template class LogicThread<AuthoritySim, RollbackSim, GameFrame>;
extern template class LogicThread<OwnerSim, RollbackSim, GameFrame>;
#endif
#endif

/** @} */
