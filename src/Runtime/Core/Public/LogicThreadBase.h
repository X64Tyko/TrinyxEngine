#pragma once
#include <thread>
#include <atomic>
#include <vector>
#include <cstdint>
#include <limits>

#include "ConstructBatch.h"
#include "NetTypes.h"
#include "TrinyxJobs.h"
#include "Types.h"

class ConstructRegistry;
class Registry;
class JoltPhysics;
class CameraManager;
class ComponentCacheBase;
struct EngineConfig;
struct InputBuffer;
struct RollbackSim; // RollbackPolicy.h — needs field access via friend

/**
 * LogicThreadBase — non-template base class for all LogicThread<> specializations.
 *
 * Owns all shared data fields and inline accessors.
 * External code (RendererCore, OwnerNet, EditorContext) holds LogicThreadBase*
 * to avoid depending on the concrete template instantiation.
 */
class LogicThreadBase
{
public:
	LogicThreadBase()          = default;
	virtual ~LogicThreadBase() = default;

	LogicThreadBase(const LogicThreadBase&)            = delete;
	LogicThreadBase& operator=(const LogicThreadBase&) = delete;

	// --- Virtual lifecycle methods (same signature for all specializations) ---
	virtual void Initialize(Registry* registry, const EngineConfig* config, JoltPhysics* physics,
							InputBuffer* simInput, InputBuffer* vizInput,
							TrinyxJobs::WorldQueueHandle worldQueue,
							const std::atomic<bool>* jobsInitialized,
							int windowWidth, int windowHeight) = 0;

	virtual void Start() = 0;
	virtual void Stop() = 0;
	virtual void Join() = 0;

	/// Run one complete simulation frame synchronously on the calling thread.
	/// Designed for editor use after loading a level, before the background
	/// thread starts.  Advances Accumulator, runs constructs, writes a temporal
	/// frame, and increments LastCompletedFrame.
	virtual void TickOnce() = 0;

	// --- Non-virtual inline accessors ---

	bool IsRunning() const { return bIsRunning.load(std::memory_order_relaxed); }

	/// Returns true while ExecuteRollback is running a resimulation pass.
	bool IsResimulating() const { return bRollbackActive; }

	/// Request a rollback to targetFrame. Thread-safe.
	void RequestRollback(uint32_t f)
	{
		PendingRollbackFrame.store(f, std::memory_order_release);
	}

	/// Request a rollback determinism test (F5 in editor). Thread-safe.
	void RequestRollbackTest()
	{
		bRollbackTestRequested.store(true, std::memory_order_release);
	}

	uint32_t GetLastCompletedFrame() const
	{
		return LastCompletedFrame.load(std::memory_order_acquire);
	}

	/// Relaxed load — slightly stale alpha is acceptable for interpolation.
	double GetAccumulator() const { return Accumulator.load(std::memory_order_relaxed); }

	double GetFixedAlpha() const
	{
		if (FixedStepTimeCache <= 0.0) return 0.0;
		const double acc = Accumulator.load(std::memory_order_relaxed);
		return (FixedStepTimeCache - acc) / FixedStepTimeCache;
	}

	void SetSimPaused(bool paused) { bSimPaused.store(paused, std::memory_order_release); }
	bool IsSimPaused() const { return bSimPaused.load(std::memory_order_acquire); }

	void SetConstructRegistry(ConstructRegistry* cr) { ConstructsPtr = cr; }

	void SetFreeFlyCamera(SimFloat x, SimFloat y, SimFloat z, SimFloat yaw, SimFloat pitch)
	{
		CamPos   = Vector3{x, y, z};
		CamYaw   = yaw;
		CamPitch = pitch;
	}

	void SetLocalCameraManager(CameraManager* mgr) { LocalCameraManager = mgr; }

	// FPS snapshots (updated ~once/second by TrackFPS; relaxed reads acceptable).
	float GetLogicFPS() const { return LogicFPS.load(std::memory_order_relaxed); }
	float GetLogicFrameMs() const { return LogicFrameMs.load(std::memory_order_relaxed); }
	float GetFixedFPS() const { return FixedFPS.load(std::memory_order_relaxed); }
	float GetFixedFrameMs() const { return FixedFrameMs.load(std::memory_order_relaxed); }

	// --- ConstructBatch --- public so Constructs can register themselves
	ConstructBatch ScalarPrePhysicsBatch;
	ConstructBatch ScalarPostPhysicsBatch;
	ConstructBatch ScalarUpdateBatch;
	ConstructBatch ScalarPhysicsStepBatch;

protected:
	friend struct RollbackSim;

	// References (non-owning)
	Registry* RegistryPtr                 = nullptr;
	const EngineConfig* ConfigPtr         = nullptr;
	JoltPhysics* PhysicsPtr               = nullptr;
	ComponentCacheBase* TemporalCache     = nullptr;
	ConstructRegistry* ConstructsPtr      = nullptr;
	CameraManager* LocalCameraManager     = nullptr;
	TrinyxJobs::WorldQueueHandle WQHandle = TrinyxJobs::InvalidWorldQueue;
	const std::atomic<bool>* JobsInitPtr  = nullptr;

	// Input (non-owning)
	InputBuffer* SimInput = nullptr;
	InputBuffer* VizInput = nullptr;

	// Camera state (FPS-style: yaw around Y, pitch around X)
	Vector3 CamPos{0.0f, 0.0f, 0.0};
	SimFloat CamYaw   = 0.0f;
	SimFloat CamPitch = 0.0f;

	// Last published camera state — used as Prev* in the next frame's header so temporal
	// interpolation spans exactly 1 fixed step, not 3 (the ring-buffer slot period).
	Vector3  LastPubCamPos{};
	Quat     LastPubCamRot{};
	SimFloat LastPubCamFoV = SimFloat(60.0f);

	static constexpr SimFloat CamMoveSpeed = SimFloat(20.0f);
	static constexpr SimFloat CamMouseSens = SimFloat(0.002f);

	// Frame mailbox — last completed frame RenderThread can read
	std::atomic<uint32_t> LastCompletedFrame{0};

	// Threading
	std::thread Thread;
	std::atomic<bool> bIsRunning{false};
	std::atomic<bool> bSimPaused{false};

	// Timing
	std::atomic<double> Accumulator{0.0};
	double SimulationTime     = 0.0;
	uint32_t FrameNumber      = 0;
	uint32_t PhysicsDivizor   = 1;
	int WindowWidth           = 1920;
	int WindowHeight          = 1080;
	double FixedStepTimeCache = 0.0; // cached from config at Initialize time

	// FPS tracking (Brain writes, editor reads)
	uint32_t FpsFrameCount = 0;
	double FpsTimer        = 0.0;
	uint32_t FpsFixedCount = 0;
	double FpsFixedTimer   = 0.0;
	double LastFPSCheck    = 0.0;

	std::atomic<float> LogicFPS{0.0f};
	std::atomic<float> LogicFrameMs{0.0f};
	std::atomic<float> FixedFPS{0.0f};
	std::atomic<float> FixedFrameMs{0.0f};

	// Rollback boundary — always present, cheap.
	// bRollbackActive is logic-thread-only (no sync needed).
	// PendingRollbackFrame and bRollbackTestRequested are written from any thread.
	bool bRollbackActive{false};
	std::atomic<uint32_t> PendingRollbackFrame{UINT32_MAX};
	std::atomic<bool> bRollbackTestRequested{false};
};
