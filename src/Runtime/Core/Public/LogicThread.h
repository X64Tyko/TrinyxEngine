#pragma once
#include <thread>
#include <atomic>
#include <functional>

#include "ConstructBatch.h"
#include "NetTypes.h"
#include "Registry.h"
#include "TrinyxJobs.h"
#include "TrinyxMPMCRing.h"
#include "TrinyxMPSCRing.h"
#include "Types.h"

// Forward declarations
class ConstructRegistry;
class Registry;
class JoltPhysics;
class CameraManager;
struct EngineConfig;
struct InputBuffer;

/**
 * LogicThread: The Brain
 *
 * Runs simulation at FixedUpdateHz with accumulator/substepping.
 * Publishes completed simulation state into the SoA slab (TemporalFrameHeader + field arrays).
 * Render thread reads the last completed slab frame independently via PropagateFrame.
 */
class LogicThread
{
public:
	LogicThread()  = default;
	~LogicThread() = default;

	void Initialize(Registry* registry, const EngineConfig* config, JoltPhysics* physics,
					InputBuffer* simInput, InputBuffer* vizInput,
					TrinyxMPSCRing<NetInputFrame>* inputAccumRing,
					const std::atomic<bool>* inputAccumEnabled,
					TrinyxJobs::WorldQueueHandle worldQueue, const std::atomic<bool>* jobsInitialized,
					int windowWidth, int windowHeight);
	void Start();
	void Stop();
	void Join();

	bool IsRunning() const { return bIsRunning.load(std::memory_order_relaxed); }
	void SetConstructRegistry(ConstructRegistry* cr) { ConstructsPtr = cr; }

	/// Returns true while ExecuteRollback is running a resimulation pass.
	/// Safe to call from Constructs (they tick on the logic thread). Used to
	/// suppress side-effectful logic (input injection, timers, logging) that
	/// should only run during the authoritative forward pass.
#ifdef TNX_ENABLE_ROLLBACK
	bool IsResimulating() const { return bRollbackActive; }
#else
	bool IsResimulating() const { return false; }
#endif

	/// Server-side hook: inject per-player input from PlayerInputLog into each player's
	/// InputBuffer before gameplay logic runs. Called each fixed tick inside ProcessSimInput.
	/// Wire up after both LogicThread and AuthorityNetThread are initialized.
	/// Returns true if the sim should stall (at least one player's input window hasn't arrived).
	/// Signature: bool(uint32_t frameNumber)
	void SetPlayerInputInjector(std::function<bool(uint32_t)> injector)
	{
		PlayerInputInjector = std::move(injector);
	}

	// Camera manager for the local Owner Soul — when set and has active layers,
	// overrides free-fly in PublishCompletedFrame.
	void SetLocalCameraManager(CameraManager* mgr) { LocalCameraManager = mgr; }

	/// Set the free-fly camera position/orientation (fallback when no camera layers are active).
	void SetFreeFlyCamera(float x, float y, float z, float yaw, float pitch)
	{
		CamPos   = {x, y, z};
		CamYaw   = yaw;
		CamPitch = pitch;
	}

	// Simulation pause — when paused, camera/input/frame publishing still run
	// but PrePhysics/PostPhysics/ScalarUpdate/physics are skipped.
	void SetSimPaused(bool paused) { bSimPaused.store(paused, std::memory_order_release); }
	bool IsSimPaused() const { return bSimPaused.load(std::memory_order_acquire); }

	// Mailbox access for RenderThread - returns the last completed frame number
	uint32_t GetLastCompletedFrame() const { return LastCompletedFrame.load(std::memory_order_acquire); }

	// Allow RenderThread to peek at accumulator for interpolation alpha calculation.
	// Relaxed load is intentional: a slightly stale alpha produces a barely-visible
	// interpolation error and is far preferable to synchronization overhead.
	double GetAccumulator() const { return Accumulator.load(std::memory_order_relaxed); }
	double GetFixedAlpha() const;
	bool TickPause(uint64_t perfFrequency, uint64_t frameStartCounter, double dt);
	void PhysicsLoop(SimFloat fixedStepTime);
	bool FixedUpdate(uint64_t perfFrequency, SimFloat fixedStepTime, int MaxPhysSubSteps, uint64_t frameStartCounter);

	// Editor-readable FPS snapshots (updated once per second by TrackFPS).
	// Relaxed loads — a stale value is harmless for a stats display.
	float GetLogicFPS() const { return LogicFPS.load(std::memory_order_relaxed); }
	float GetLogicFrameMs() const { return LogicFrameMs.load(std::memory_order_relaxed); }
	float GetFixedFPS() const { return FixedFPS.load(std::memory_order_relaxed); }
	float GetFixedFrameMs() const { return FixedFrameMs.load(std::memory_order_relaxed); }

	// Construct scalar tick batches — executed after the corresponding wide entity sweeps
	ConstructBatch ScalarPrePhysicsBatch;
	ConstructBatch ScalarPostPhysicsBatch;
	ConstructBatch ScalarUpdateBatch;
	ConstructBatch ScalarPhysicsStepBatch; // Runs during flush window (after Step completes)

#ifdef TNX_ENABLE_ROLLBACK
	// Called from Sentinel thread (PumpEvents) on F5 press.
	void RequestRollbackTest() { bRollbackTestRequested.store(true, std::memory_order_release); }
	void ProcessRollback();

	// Called from any thread (e.g. net reconciliation) to trigger a production rollback.
	// targetFrame is the authoritative frame to rewind to; resim runs to current FrameNumber.
	void RequestRollback(uint32_t targetFrame)
	{
		PendingRollbackFrame.store(targetFrame, std::memory_order_release);
	}

	uint32_t GetPhysicsDivizor() const { return PhysicsDivizor; }

	// Called from the Logic thread (via PostAndWait) when a state correction arrives.
	// Queues server-authoritative transforms for comparison during resim and triggers
	// a rollback to earliestClientFrame so the sim can re-derive correct state.
	void EnqueueCorrections(std::vector<EntityTransformCorrection> corrections, uint32_t earliestClientFrame);

	// Called when a state correction arrives for a frame the client hasn't reached yet.
	// Applied inline during PhysicsLoop when FrameNumber == correction.ClientFrame — no rollback.
	void EnqueuePredictedCorrections(std::vector<EntityTransformCorrection> corrections);

	/// Request a rollback to clientFrame so newly-received spawns are inserted at their
	/// correct historical ring slot. Thread-safe: called from the net thread after a
	/// spawn handshake completes. Merges with any correction-driven rollback — the logic
	/// thread takes whichever target is oldest.
	void EnqueueSpawnRollback(uint32_t clientFrame);
#endif

private:
	void ThreadMain(); // Thread entry point

	// Lifecycle Methods
	void ProcessVizInput(SimFloat dt); // Swap FizInput buffer, update camera from WASD + mouse
	bool ProcessSimInput(SimFloat dt); // Swap SimInput, call injector; returns true if input-stalled
	void ScalarUpdate(SimFloat dt);    // Variable update (runs every frame)
	void PrePhysics(SimFloat dt);      // Fixed update at FixedUpdateHz
	void PostPhysics(SimFloat dt);     // Fixed update at FixedUpdateHz

	void PublishCompletedFrame(); // Publish last written frame number to mailbox
	void WaitForTiming(uint64_t frameStart, uint64_t perfFrequency);
	void TrackFPS();

	// References (non-owning)
	Registry* RegistryPtr                   = nullptr;
	const EngineConfig* ConfigPtr           = nullptr;
	JoltPhysics* PhysicsPtr                 = nullptr;
	class ComponentCacheBase* TemporalCache = nullptr;
	ConstructRegistry* ConstructsPtr        = nullptr;
	CameraManager* LocalCameraManager       = nullptr;
	TrinyxJobs::WorldQueueHandle WQHandle   = TrinyxJobs::InvalidWorldQueue;
	const std::atomic<bool>* JobsInitPtr    = nullptr;

	// Input
	InputBuffer* SimInput = nullptr;
	InputBuffer* VizInput = nullptr;
	TrinyxMPSCRing<NetInputFrame>* InputAccumRing = nullptr; // non-owning; client-side only
	const std::atomic<bool>* InputAccumEnabled    = nullptr; // gated at PlayerBeginConfirm

	// Server-side: per-player input injection hook (null on clients/standalone)
	// Returns true if the sim should stall this tick (at least one player is behind).
	std::function<bool(uint32_t)> PlayerInputInjector;

	// Camera state (FPS-style: yaw around Y, pitch around X)
	Vector3 CamPos{0.0f, 0.0f, 0.0f};
	float CamYaw   = 0.0f; // radians, 0 = looking down -Z
	float CamPitch = 0.0f; // radians, positive = looking up

	static constexpr float CamMoveSpeed = 20.0f;  // units/sec
	static constexpr float CamMouseSens = 0.002f; // radians/pixel

	// Frame mailbox - last completed frame number that RenderThread can read
	std::atomic<uint32_t> LastCompletedFrame{0};

	// Threading
	std::thread Thread;
	std::atomic<bool> bIsRunning{false};
	std::atomic<bool> bSimPaused{false};

	// Timing — atomic to allow safe relaxed reads from the RenderThread
	std::atomic<double> Accumulator{0.0};
	double SimulationTime   = 0.0;
	uint32_t FrameNumber    = 0;
	uint32_t PhysicsDivizor = 1;
	int WindowWidth         = 1920;
	int WindowHeight        = 1080;

	// FPS tracking (Brain thread writes, editor reads via atomics)
	uint32_t FpsFrameCount = 0;
	double FpsTimer        = 0.0;
	uint32_t FpsFixedCount = 0;
	double FpsFixedTimer   = 0.0;
	double LastFPSCheck    = 0.0;

	// Snapshot values published for editor consumption
	std::atomic<float> LogicFPS{0.0f};
	std::atomic<float> LogicFrameMs{0.0f};
	std::atomic<float> FixedFPS{0.0f};
	std::atomic<float> FixedFrameMs{0.0f};

#ifdef TNX_ENABLE_ROLLBACK
	// --- Rollback ---
	std::atomic<bool> bRollbackTestRequested{false};
	std::atomic<uint32_t> PendingRollbackFrame{UINT32_MAX};
	bool bRollbackActive{false}; // Logic-thread-only guard: set during ExecuteRollback
	static constexpr uint32_t RollbackFrameCount = 5;

	std::vector<EntityTransformCorrection> PendingCorrections; // Logic-thread-only

	// MPMC ring buffer: worker/net threads push corrections here; logic thread drains
	// into PendingCorrections at the start of each rollback check. Sized for 256 entries
	// (~10× the expected burst at 23Hz replication with up to 24 entities per packet).
	TrinyxMPMCRing<EntityTransformCorrection> IncomingCorrections;

	// Corrections for frames the client hasn't processed yet (clientFrame > LastAckedFrame).
	// Applied inline during PhysicsLoop when FrameNumber reaches the correction's target frame.
	// Never trigger a rollback — the forward pass hasn't written that frame yet.
	TrinyxMPMCRing<EntityTransformCorrection> IncomingPredictedCorrections;
	std::queue<EntityTransformCorrection> PendingPredictedCorrections; // Logic-thread-only

	void ExecuteRollback(uint32_t targetFrame);   // Production rewind+resim — no test scaffolding
	void ExecuteRollbackTest();                    // Test wrapper: save → ExecuteRollback → compare → restore
	void RecordFrameInput();                       // Copy SimInput into current frame header
	void InjectFrameInput(uint32_t frameNum);      // Restore from frame header into SimInput

#ifdef TNX_TESTING
	// Pre-allocated backup buffers for determinism validation (test harness only)
	std::vector<uint8_t> TemporalSlabBackup;
	std::vector<uint8_t> VolatileSlabBackup;
	std::vector<uint8_t> GroundTruthBackup;
#endif // TNX_TESTING
#endif // TNX_ENABLE_ROLLBACK
};

inline void LogicThread::PrePhysics(SimFloat dt)
{
	TNX_ZONE_N("Logic_FixedUpdate");

	RegistryPtr->InvokePrePhys(dt);
}

inline void LogicThread::ScalarUpdate(SimFloat dt)
{
	TNX_ZONE_N("Logic_Update");

	RegistryPtr->InvokeScalarUpdate(dt);
}

inline void LogicThread::PostPhysics(SimFloat dt)
{
	TNX_ZONE_N("Logic_FixedUpdate");

	RegistryPtr->InvokePostPhys(dt);
}