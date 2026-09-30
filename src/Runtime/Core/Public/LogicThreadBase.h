#pragma once
#include <cassert>
#include <thread>
#include <atomic>
#include <vector>
#include <cstdint>
#include <limits>

#include "ConstructBatch.h"
#include "NetTypes.h"
#include "TrinyxJobs.h"
#include "Types.h"

/** @addtogroup core
 *  @{
 */

class ConstructRegistry;
class Registry;
class JoltPhysics;
class CameraManager;
class ComponentCacheBase;
struct EngineConfig;
struct InputBuffer;
struct RollbackSim; // needs field access via friend

/**
 * @brief Non-template base class for all LogicThread<> specializations.
 *
 * Owns all shared data fields and provides type-erased accessors for external
 * systems (RendererCore, OwnerNet, EditorContext) that do not need the concrete
 * template instantiation — they hold @c LogicThreadBase* instead.
 */
class LogicThreadBase
{
public:
	LogicThreadBase()          = default;
	virtual ~LogicThreadBase() = default;

	LogicThreadBase(const LogicThreadBase&)            = delete;
	LogicThreadBase& operator=(const LogicThreadBase&) = delete;

	/**
	 * @brief Bind all non-owning pointers. Called by WorldBase::InitBase before Start().
	 * @param worldQueue      World-scoped job queue; Brain submits pre/post-physics jobs here.
	 * @param jobsInitialized Engine jobs-ready gate; Brain spins on this before draining the queue.
	 */
	virtual void Initialize(Registry* registry, const EngineConfig* config, JoltPhysics* physics,
		InputBuffer* simInput, InputBuffer* vizInput,
		TrinyxJobs::WorldQueueHandle worldQueue,
		const std::atomic<bool>* jobsInitialized,
		int windowWidth, int windowHeight) = 0;

	virtual void Start() = 0; ///< @brief Spawn the Brain thread and begin the fixed-rate loop.
	virtual void Stop()  = 0; ///< @brief Signal the Brain thread to exit after the current frame.
	virtual void Join()  = 0; ///< @brief Block the caller until the Brain thread exits.

	/**
	 * @brief Run one complete simulation frame synchronously on the calling thread.
	 *
	 * Intended for editor use after level load, before the background thread starts.
	 * Advances the accumulator, runs Constructs, writes a temporal frame, and increments
	 * @c LastCompletedFrame.
	 */
	virtual void TickOnce() = 0;

	// --- Non-virtual inline accessors ---

	/// @brief Returns true while the Brain thread is running.
	bool IsRunning() const { return bIsRunning.load(std::memory_order_relaxed); }

	/// @brief Returns true while ExecuteRollback is actively resimulating frames.
	bool IsResimulating() const { return bRollbackActive; }

	/// @brief Request a rollback to @p f on the next tick. Thread-safe.
	void RequestRollback(uint32_t f)
	{
		PendingRollbackFrame.store(f, std::memory_order_release);
	}

	/// @brief Request a rollback determinism self-test (F5 in editor). Thread-safe.
	void RequestRollbackTest()
	{
#ifdef TNX_TESTING
		bRollbackTestComplete.store(false, std::memory_order_relaxed);
#endif
		bRollbackTestRequested.store(true, std::memory_order_release);
	}

#ifdef TNX_TESTING
	/// @brief True after ExecuteRollbackTest has run; reset by RequestRollbackTest(). Thread-safe.
	bool IsRollbackTestComplete() const { return bRollbackTestComplete.load(std::memory_order_acquire); }
	/// @brief True if the most recent rollback test was byte-perfect. Valid after IsRollbackTestComplete(). Thread-safe.
	bool GetRollbackTestPassed() const { return bRollbackTestPassed.load(std::memory_order_acquire); }
#endif

	/// @brief Last fully-published sim frame number. Render thread uses this for interpolation.
	uint32_t GetLastCompletedFrame() const
	{
		return LastCompletedFrame.load(std::memory_order_acquire);
	}

	/// @brief Current accumulator depth.
	/// @note Relaxed load — slightly stale alpha is acceptable for visual interpolation.
	double GetAccumulator() const { return Accumulator.load(std::memory_order_relaxed); }

	/// @brief Normalised interpolation alpha [0, 1] between the last two completed fixed steps.
	double GetFixedAlpha() const
	{
		if (FixedStepTimeCache <= 0.0) return 0.0;
		const double acc = Accumulator.load(std::memory_order_relaxed);
		return (FixedStepTimeCache - acc) / FixedStepTimeCache;
	}

	/// @brief Pause or resume the fixed-rate sim loop. Thread-safe.
	void SetSimPaused(bool paused) { bSimPaused.store(paused, std::memory_order_release); }
	/// @brief Returns true while the sim loop is paused.
	bool IsSimPaused() const { return bSimPaused.load(std::memory_order_acquire); }
	/// @brief Frame number at which the sim was paused. Valid while IsSimPaused(). Editor scrubber uses this as the rewind target.
	uint32_t GetPausedAtFrame() const { return PausedAtFrame.load(std::memory_order_acquire); }

	/// @brief Inject the ConstructRegistry so Constructs can self-register at spawn time.
	void SetConstructRegistry(ConstructRegistry* cr) { ConstructsPtr = cr; }

	/// @brief Expose the ConstructRegistry for editor inspection (read-only).
	ConstructRegistry* GetConstructRegistry() const { return ConstructsPtr; }

	/// @brief Set the free-fly camera position and orientation for editor/debug use.
	void SetFreeFlyCamera(SimFloat x, SimFloat y, SimFloat z, SimFloat yaw, SimFloat pitch)
	{
		CamPos   = Vector3{ x, y, z };
		CamYaw   = yaw;
		CamPitch = pitch;
	}

	/// @brief Inject the per-Soul CameraManager so the Brain resolves the active camera each frame.
	void SetLocalCameraManager(CameraManager* mgr) { LocalCameraManager = mgr; }

	/**
	 * @brief Set the earliest frame a spawn-triggered rollback may target.
	 *
	 * Must be called once per level load on the Logic thread (inside SpawnAndWait), with a
	 * frame value strictly greater than the previous value. Prevents rollbacks from targeting
	 * frames whose Jolt snapshots predate the current level geometry.
	 */
	void SetEarliestValidRollbackFrame(uint32_t frame)
	{
		assert(frame != 0 && "EarliestValidRollbackFrame must be a real frame number");
		[[maybe_unused]] const uint32_t prev = EarliestValidRollbackFrame.load(std::memory_order_relaxed);
		assert(frame > prev && "EarliestValidRollbackFrame must advance monotonically");
		EarliestValidRollbackFrame.store(frame, std::memory_order_relaxed);
	}

	/// @note Updated ~once/second by TrackFPS; relaxed reads are acceptable for display.
	float GetLogicFPS() const { return LogicFPS.load(std::memory_order_relaxed); }
	float GetLogicFrameMs() const { return LogicFrameMs.load(std::memory_order_relaxed); }
	float GetFixedFPS() const { return FixedFPS.load(std::memory_order_relaxed); }
	float GetFixedFrameMs() const { return FixedFrameMs.load(std::memory_order_relaxed); }

	// --- Construct tick batches — public so Constructs can self-register at spawn time ---
	ConstructBatch ScalarPrePhysicsBatch;  ///< Runs before PrePhysics wide sweeps each fixed step.
	ConstructBatch ScalarPostPhysicsBatch; ///< Runs after PostPhysics wide sweeps each fixed step.
	ConstructBatch ScalarUpdateBatch;      ///< Runs each variadic (uncapped) logic tick.
	ConstructBatch ScalarPhysicsStepBatch; ///< Runs between FlushPendingBodies and the Jolt step.

protected:
	friend struct RollbackSim;

	// --- Non-owning references — bound at Initialize time ---
	Registry* RegistryPtr                 = nullptr;
	const EngineConfig* ConfigPtr         = nullptr;
	JoltPhysics* PhysicsPtr               = nullptr;
	ComponentCacheBase* TemporalCache     = nullptr;
	ConstructRegistry* ConstructsPtr      = nullptr;
	CameraManager* LocalCameraManager     = nullptr;
	TrinyxJobs::WorldQueueHandle WQHandle = TrinyxJobs::InvalidWorldQueue;
	const std::atomic<bool>* JobsInitPtr  = nullptr;

	InputBuffer* SimInput = nullptr; ///< Non-owning; WorldBase owns the buffer.
	InputBuffer* VizInput = nullptr; ///< Non-owning; WorldBase owns the buffer.

	// --- Camera state (FPS-style: yaw around Y, pitch around X) ---
	Vector3 CamPos{ 0.0f, 0.0f, 0.0 };
	SimFloat CamYaw   = 0.0f;
	SimFloat CamPitch = 0.0f;

	// Last published camera state — used as Prev* in the next frame header so temporal
	// interpolation spans exactly 1 fixed step, not 3 (the ring-buffer slot period).
	Vector3 LastPubCamPos{};
	Quat LastPubCamRot{};
	SimFloat LastPubCamFoV = SimFloat(60.0f);

	static constexpr SimFloat CamMoveSpeed = SimFloat(20.0f);  ///< Free-fly camera speed (sim units/s).
	static constexpr SimFloat CamMouseSens = SimFloat(0.002f); ///< Free-fly mouse sensitivity (rad/pixel).

	std::atomic<uint32_t> LastCompletedFrame{ 0 }; ///< Frame mailbox; last completed frame the Render thread may read.

	// --- Threading ---
	std::thread Thread;
	std::atomic<bool> bIsRunning{ false };
	std::atomic<bool> bSimPaused{ false };
	std::atomic<uint32_t> PausedAtFrame{ 0 };

	// --- Timing ---
	std::atomic<double> Accumulator{ 0.0 };
	double SimulationTime     = 0.0;
	uint32_t FrameNumber      = 0;
	uint32_t PhysicsDivizor   = 1;
	int WindowWidth           = 1920;
	int WindowHeight          = 1080;
	double FixedStepTimeCache = 0.0; ///< Cached from EngineConfig at Initialize time.

	// --- FPS tracking (Brain writes, editor reads) ---
	uint32_t FpsFrameCount = 0;
	double FpsTimer        = 0.0;
	uint32_t FpsFixedCount = 0;
	double FpsFixedTimer   = 0.0;
	double LastFPSCheck    = 0.0;

	std::atomic<float> LogicFPS{ 0.0f };
	std::atomic<float> LogicFrameMs{ 0.0f };
	std::atomic<float> FixedFPS{ 0.0f };
	std::atomic<float> FixedFrameMs{ 0.0f };

	// --- Rollback state ---
	// bRollbackActive is Logic-thread-only (no sync needed).
	// All other rollback atomics are written from any thread.
	bool bRollbackActive{ false };
	std::atomic<uint32_t> PendingRollbackFrame{ UINT32_MAX };
	std::atomic<bool> bRollbackTestRequested{ false };
#ifdef TNX_TESTING
	std::atomic<bool> bRollbackTestComplete{ false };
	///< Set by ExecuteRollbackTest when done; cleared by RequestRollbackTest.
	std::atomic<bool> bRollbackTestPassed{ false };
	///< True if the last test was byte-perfect; valid after bRollbackTestComplete.
#endif

	/// Hard floor for spawn rollback targets — set once per level load so rollbacks never
	/// target a frame whose Jolt snapshot predates the level geometry. Written on the Logic
	/// thread; relaxed ordering is sufficient because visibility is guaranteed by the spawn
	/// handshake that precedes any EnqueueSpawnRollback call.
	std::atomic<uint32_t> EarliestValidRollbackFrame{ 0 };
};

/** @} */
