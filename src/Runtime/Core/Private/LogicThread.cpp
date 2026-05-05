#include "LogicThread.h"
#include "QuatMath.h"

// ---------------------------------------------------------------------------
// Template aliases for readability in method signatures
// ---------------------------------------------------------------------------
#define TMPL template <typename TNet, typename TRollback, typename TFrame>
#define LogicThread   LogicThread<TNet, TRollback, TFrame>

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------
TMPL
void LogicThread::Initialize(Registry* registry, const EngineConfig* config, JoltPhysics* physics,
							 InputBuffer* simInput, InputBuffer* vizInput,
							 TrinyxJobs::WorldQueueHandle worldQueue,
							 const std::atomic<bool>* jobsInitialized,
							 int windowWidth, int windowHeight)
{
	RegistryPtr        = registry;
	ConfigPtr          = config;
	PhysicsPtr         = physics;
	SimInput           = simInput;
	VizInput           = vizInput;
	WQHandle           = worldQueue;
	JobsInitPtr        = jobsInitialized;
	TemporalCache      = registry->GetTemporalCache();
	WindowWidth        = windowWidth;
	WindowHeight       = windowHeight;
	PhysicsDivizor     = config->PhysicsUpdateInterval;
	FixedStepTimeCache = config->GetFixedStepTime();

	LastCompletedFrame.store(0, std::memory_order_release);

	Rollback.InitializeRings();

	LOG_ENG_INFO("[LogicThread] Initialized");
}

// ---------------------------------------------------------------------------
// Start / Stop / Join
// ---------------------------------------------------------------------------
TMPL
void LogicThread::Start()
{
	bIsRunning.store(true, std::memory_order_release);
	Thread = std::thread(&LogicThread::ThreadMain, this);
	TrinyxThreading::PinThread(Thread);
	LOG_ENG_INFO("[LogicThread] Started");
}

TMPL
void LogicThread::Stop()
{
	bIsRunning.store(false, std::memory_order_release);
	LOG_ENG_INFO("[LogicThread] Stop requested");
}

TMPL
void LogicThread::Join()
{
	if (Thread.joinable())
	{
		Thread.join();
		LOG_ENG_INFO("[LogicThread] Joined");
	}
}

// ---------------------------------------------------------------------------
// TickPause (editor path)
// ---------------------------------------------------------------------------
TMPL
bool LogicThread::TickPause(const uint64_t perfFrequency, const uint64_t frameStartCounter, double dt)
{
	if (bSimPaused.load(std::memory_order_acquire)) [[unlikely]]
	{
		ProcessSimInput(static_cast<SimFloat>(dt));
		ProcessVizInput(static_cast<SimFloat>(dt));
		PublishCompletedFrame();
		RegistryPtr->PropagateFrame(FrameNumber++);

		WaitForTiming(frameStartCounter, perfFrequency);
		TrackFPS();
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// PhysicsLoop
// ---------------------------------------------------------------------------
TMPL
void LogicThread::PhysicsLoop(const SimFloat fixedStepTime)
{
	PrePhysics(fixedStepTime);
	ScalarPrePhysicsBatch.Execute(fixedStepTime);

	if (FrameNumber % PhysicsDivizor == 0)
	{
		PhysicsPtr->PullActiveTransforms(RegistryPtr);
		
		PhysicsPtr->FlushPendingBodies(RegistryPtr);
		PhysicsPtr->PushKinematicTransforms(RegistryPtr, fixedStepTime.ToFloat());
		ScalarPhysicsStepBatch.Execute(fixedStepTime * PhysicsDivizor);
		
		PhysicsPtr->ProcessContacts(RegistryPtr);
		Rollback.SaveSnapshot(*this);
		
		TrinyxJobs::Dispatch([this, fixedStepTime](uint32_t)
		{
			PhysicsPtr->Step(fixedStepTime.ToFloat() * PhysicsDivizor);
		}, PhysicsPtr->GetJoltPhysCounter(), TrinyxJobs::Queue::Physics);
	}

	PostPhysics(fixedStepTime);
	ScalarPostPhysicsBatch.Execute(fixedStepTime);

	Rollback.ApplyPredictedCorrections(*this);
}

// ---------------------------------------------------------------------------
// FixedUpdate
// ---------------------------------------------------------------------------
TMPL
bool LogicThread::FixedUpdate(const uint64_t perfFrequency, const SimFloat fixedStepTime,
							  const int MaxPhysSubSteps, const uint64_t frameStartCounter)
{
	if (Accumulator.load(std::memory_order_relaxed) <= 0) [[unlikely]]
	{
		TNX_ZONE_NC("Physics Loop", TNX_COLOR_LOGIC);

		bool bDidStall = false;
		int steps      = 0;
		while (Accumulator.load(std::memory_order_relaxed) <= 0 && steps < MaxPhysSubSteps)
		{
			const bool bInputStalled = ProcessSimInput(fixedStepTime);
			if (bInputStalled)
			{
				bDidStall = true;
				break;
			}

			FpsFixedCount++;
			FpsFixedTimer += fixedStepTime.ToDouble();

			Rollback.RecordFrameInput(*this);

			PhysicsLoop(fixedStepTime);

			Accumulator.store(Accumulator.load(std::memory_order_relaxed) + fixedStepTime.ToDouble(),
							  std::memory_order_relaxed);
			++steps;
			SimulationTime += fixedStepTime.ToDouble();

			ProcessVizInput(fixedStepTime);

			Rollback.ReplayServerEvents(*this);

			PublishCompletedFrame();

			// Notify the net policy that this frame is fully committed before PropagateFrame
			// increments FrameNumber. AuthoritySim uses this to advance CommittedFrameHorizon.
			NetMode.OnFramePublished(FrameNumber, *this);

			RegistryPtr->PropagateFrame(FrameNumber++);

			Rollback.ProcessRollback(*this);
		}

		TrackFPS();
		if (bDidStall || ConfigPtr->TargetFPS > 0) WaitForTiming(frameStartCounter, perfFrequency);
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// ThreadMain
// ---------------------------------------------------------------------------
TMPL
void LogicThread::ThreadMain()
{
	const uint64_t perfFrequency = SDL_GetPerformanceFrequency();
	uint64_t lastCounter         = SDL_GetPerformanceCounter();

	const SimFloat fixedStepTime = static_cast<SimFloat>(ConfigPtr->GetFixedStepTime());

	constexpr double MaxDt              = 0.25;
	constexpr double MaxAccumulatedTime = -0.25;
	constexpr int MaxPhysSubSteps       = 8;

	while (!JobsInitPtr->load(std::memory_order_acquire))
	{
	}

	while (bIsRunning.load(std::memory_order_acquire))
	{
		TNX_ZONE_NC("Logic Frame", TNX_COLOR_LOGIC);

		TrinyxJobs::DrainWorldQueue(WQHandle);
		RegistryPtr->ProcessDeferredDestructions();
		if (ConstructsPtr) ConstructsPtr->ProcessDeferredDestructions();
		RegistryPtr->TickDefrag(WQHandle);

		const uint64_t frameStartCounter = SDL_GetPerformanceCounter();
		uint64_t counterElapsed          = frameStartCounter - lastCounter;
		lastCounter                      = frameStartCounter;

		double dt = static_cast<double>(counterElapsed) / static_cast<double>(perfFrequency);
		if (dt > MaxDt) dt = MaxDt;

		if constexpr (TFrame::IsEditor)
		{
			if (TickPause(perfFrequency, frameStartCounter, dt)) continue;
		}

		double acc = Accumulator.load(std::memory_order_relaxed) - dt;
		if (acc < MaxAccumulatedTime) acc = MaxAccumulatedTime;
		Accumulator.store(acc, std::memory_order_relaxed);

		if (FixedUpdate(perfFrequency, fixedStepTime, MaxPhysSubSteps, frameStartCounter)) continue;

		if (dt > Accumulator.load(std::memory_order_relaxed)) [[likely]]
		{
			ScalarUpdate(static_cast<SimFloat>(dt));
			ScalarUpdateBatch.Execute(static_cast<SimFloat>(dt));
			TrackFPS();
		}

		if (ConfigPtr->TargetFPS > 0) WaitForTiming(frameStartCounter, perfFrequency);
	}

	// Drain any in-flight Jolt step before the thread exits so JoltPhysics::Shutdown
	// cannot destroy PhysSystem while a physics job is still running on a worker.
	TrinyxJobs::WaitForCounter(PhysicsPtr->GetJoltPhysCounter(), TrinyxJobs::Queue::Physics);
	PhysicsPtr->Shutdown();
}

// ---------------------------------------------------------------------------
// ProcessSimInput
// ---------------------------------------------------------------------------
TMPL
bool LogicThread::ProcessSimInput(SimFloat /*dt*/)
{
	SimInput->Swap();
	return NetMode.OnSimInput(FrameNumber, *this);
}

// ---------------------------------------------------------------------------
// ProcessVizInput
// ---------------------------------------------------------------------------
TMPL
void LogicThread::ProcessVizInput(SimFloat dt)
{
	VizInput->Swap();

	if (LocalCameraManager) LocalCameraManager->Tick(dt);

	// Free-fly fallback — skip when CameraManager has active layers.
	if (LocalCameraManager && LocalCameraManager->HasActiveLayers()) return;

	CamYaw   += VizInput->GetMouseDX() * CamMouseSens;
	CamPitch -= VizInput->GetMouseDY() * CamMouseSens;

	constexpr SimFloat MaxPitch = SimFloat(1.5533f);
	if (CamPitch > MaxPitch) CamPitch = MaxPitch;
	if (CamPitch < -MaxPitch) CamPitch = -MaxPitch;

	SimFloat sinYaw   = FastSin(CamYaw);
	SimFloat cosYaw   = FastCos(CamYaw);
	SimFloat sinPitch = FastSin(CamPitch);
	SimFloat cosPitch = FastCos(CamPitch);

	Vector3 forward{sinYaw * cosPitch, sinPitch, -cosYaw * cosPitch};
	Vector3 right{cosYaw, 0, sinYaw};
	Vector3 up{0, 1, 0};

	Vector3 moveDir{0, 0, 0};

	if (VizInput->IsActionDown(Action::MoveForward)) moveDir = moveDir + forward;
	if (VizInput->IsActionDown(Action::MoveBackward)) moveDir = moveDir - forward;
	if (VizInput->IsActionDown(Action::MoveRight)) moveDir = moveDir + right;
	if (VizInput->IsActionDown(Action::MoveLeft)) moveDir = moveDir - right;
	if (VizInput->IsActionDown(Action::MoveUp)) moveDir = moveDir + up;
	if (VizInput->IsActionDown(Action::MoveDown)) moveDir = moveDir - up;

	SimFloat moveLen = moveDir.Length();
	if (moveLen > SimFloat(0.001f)) CamPos = CamPos + moveDir * (dt * CamMoveSpeed / moveLen);
}

// ---------------------------------------------------------------------------
// ScalarUpdate / PrePhysics / PostPhysics
// ---------------------------------------------------------------------------
TMPL
void LogicThread::ScalarUpdate(SimFloat dt)
{
	TNX_ZONE_N("Logic_Update");
	RegistryPtr->InvokeScalarUpdate(dt);
}

TMPL
void LogicThread::PrePhysics(SimFloat dt)
{
	TNX_ZONE_N("Logic_FixedUpdate");
	RegistryPtr->InvokePrePhys(dt);
}

TMPL
void LogicThread::PostPhysics(SimFloat dt)
{
	TNX_ZONE_N("Logic_FixedUpdate");
	RegistryPtr->InvokePostPhys(dt);
}

// ---------------------------------------------------------------------------
// PublishCompletedFrame
// ---------------------------------------------------------------------------
TMPL
void LogicThread::PublishCompletedFrame()
{
	TNX_ZONE_N("Logic_PublishFrame");

	TemporalFrameHeader* header = TemporalCache->GetFrameHeader();

	header->FrameNumber            = FrameNumber;
	header->ActiveEntityCount      = static_cast<uint32_t>(RegistryPtr->GetTotalEntityCount());
	header->TotalAllocatedEntities = static_cast<uint32_t>(RegistryPtr->GetTotalEntityCount());

	SimFloat activeFOV = SimFloat(60.0f);
	Vector3  activePos = CamPos;
	Quat     activeRot = QuatFromYawPitch(CamYaw, CamPitch);

	if (LocalCameraManager)
	{
		WorldCameraState cs = LocalCameraManager->Resolve();
		if (cs.Valid)
		{
			activePos = cs.Position;
			activeRot = cs.Rotation;
			activeFOV = cs.FOV;
		}
	}

	// Prev* uses the last published values (1 fixed step ago), not the current ring-buffer
	// slot — which was written N_SLOTS frames ago, not 1.
	header->PrevCameraPosition = LastPubCamPos;
	header->PrevCameraRotation = LastPubCamRot;
	header->PrevCameraFoV      = LastPubCamFoV;

	header->CameraPosition = activePos;
	header->CameraRotation = activeRot;
	header->CameraFoV      = activeFOV;

	LastPubCamPos = activePos;
	LastPubCamRot = activeRot;
	LastPubCamFoV = activeFOV;
#if TNX_DEV_METRICS
	header->InputTimestamp = VizInput->GetSwapPerfCount();
#if TNX_DEV_METRICS_DETAILED
	if (VizInput->GetSwapPerfCount() != 0)
	{
		double bufferMs = static_cast<double>(VizInput->GetCurrentSwapTime() - VizInput->GetSwapPerfCount())
			/ static_cast<double>(SDL_GetPerformanceFrequency()) * 1000.0;
		LOG_ENG_DEBUG_F("[Latency] Buffer: %.2fms (input wait in swap buffer)", bufferMs);
	}
#endif
#endif

	header->SunDirection     = Vector3{0.0f, -1.0f, 0.0f};
	header->SunColor         = Vector3{1.0f, 1.0f, 1.0f};
	header->AmbientIntensity = SimFloat(0.2f);

	LastCompletedFrame.store(FrameNumber, std::memory_order_release);
	RegistryPtr->LastPublishedFrame = FrameNumber;
}

// ---------------------------------------------------------------------------
// WaitForTiming
// ---------------------------------------------------------------------------
TMPL
void LogicThread::WaitForTiming(uint64_t frameStart, uint64_t perfFrequency)
{
	TNX_ZONE_N("Logic_WaitTiming");

	const double targetFrameTimeSec = ConfigPtr->GetTargetFrameTime();
	const uint64_t targetTicks      = static_cast<uint64_t>(targetFrameTimeSec * static_cast<double>(perfFrequency));
	const uint64_t frameEnd         = frameStart + targetTicks;

	uint64_t currentCounter = SDL_GetPerformanceCounter();
	if (frameEnd > currentCounter)
	{
		const double remainingSec =
			static_cast<double>(frameEnd - currentCounter) / static_cast<double>(perfFrequency);

		constexpr double SleepMarginSec = 0.002;
		if (remainingSec > SleepMarginSec)
		{
			const double sleepSec = remainingSec - SleepMarginSec;
			SDL_Delay(static_cast<uint32_t>(sleepSec * 1000.0));
		}

		while (SDL_GetPerformanceCounter() < frameEnd)
		{
			/* busy wait */
		}
	}
}

// ---------------------------------------------------------------------------
// TrackFPS
// ---------------------------------------------------------------------------
TMPL
void LogicThread::TrackFPS()
{
	FpsFrameCount++;
	const double now = SDL_GetPerformanceCounter() /
		static_cast<double>(SDL_GetPerformanceFrequency());
	FpsTimer     += now - LastFPSCheck;
	LastFPSCheck = now;

	if (FpsTimer >= 1.0) [[unlikely]]
	{
		float fps = static_cast<float>(FpsFrameCount / FpsTimer);
		float ms  = static_cast<float>((FpsTimer / FpsFrameCount) * 1000.0);
		LogicFPS.store(fps, std::memory_order_relaxed);
		LogicFrameMs.store(ms, std::memory_order_relaxed);
		LOG_ENG_DEBUG_F("Logic FPS: %d | Frame: %.2fms", static_cast<int>(fps), static_cast<double>(ms));
		FpsFrameCount = 0;
		FpsTimer      = 0.0;
	}

	if (FpsFixedTimer >= 1.0) [[unlikely]]
	{
		float ffps = static_cast<float>(FpsFixedCount / FpsFixedTimer);
		float fms  = static_cast<float>((FpsFixedTimer / FpsFixedCount) * 1000.0);
		FixedFPS.store(ffps, std::memory_order_relaxed);
		FixedFrameMs.store(fms, std::memory_order_relaxed);
		LOG_ENG_DEBUG_F("Fixed FPS: %d | Frame: %.2fms", static_cast<int>(ffps), static_cast<double>(fms));
		FpsFixedCount = 0;
		FpsFixedTimer = 0.0;
	}
}

// ---------------------------------------------------------------------------
// TickOnce — synchronous one-frame tick for editor
// ---------------------------------------------------------------------------
TMPL
void LogicThread::TickOnce()
{
	// Ensure no background thread is running (we are single-threaded here)
	if (bIsRunning.load(std::memory_order_acquire)) return; // already running; don't interfere

	const double fixedStep = ConfigPtr->GetFixedStepTime();

	// Process deferred removals
	RegistryPtr->ProcessDeferredDestructions();
	if (ConstructsPtr) ConstructsPtr->ProcessDeferredDestructions();

	// Swap input buffers once (with zero simulated time)
	SimInput->Swap();
	VizInput->Swap();

	// Accumulate exactly one fixed step
	double acc = Accumulator.load(std::memory_order_relaxed);
	acc        -= fixedStep;
	if (acc < -0.25) acc = -0.25;
	Accumulator.store(acc, std::memory_order_relaxed);

	// Do one fixed step using the same internal loop
	// We'll call FixedUpdate with dt = fixedStep. It will run once because acc <= 0.
	uint64_t dummy = SDL_GetPerformanceCounter();
	FixedUpdate(dummy, static_cast<SimFloat>(fixedStep), 1, dummy);

	// After FixedUpdate, the accumulator is 0 or positive because it did one step.
	// Now run the slow update path (like the main loop does after FixedUpdate returns false)
	ScalarUpdate(static_cast<SimFloat>(fixedStep));
	ScalarUpdateBatch.Execute(static_cast<SimFloat>(fixedStep));

	// The temporal frame is already written by PublishCompletedFrame inside FixedUpdate.
	// LastCompletedFrame is increased.
}


// ---------------------------------------------------------------------------
// Explicit instantiations — compile all three specializations
// ---------------------------------------------------------------------------
// OwnerSim uses RollbackSim only when rollback history is enabled at build time.
// When disabled, the temporal ring stores in the volatile slab and rollback
// code paths are dead — NoRollback avoids instantiating the rollback-specific
// API (InputKeyState, SetActiveWriteFrame, Jolt snapshots) that are also
// gated by TNX_ENABLE_ROLLBACK in the engine headers.
// Explicit instantiations for LogicThread — must come after #undef to avoid
// the macro expanding ::LogicThread into the wrong token sequence.
#undef TMPL
#undef LogicThread

template class ::LogicThread<SoloSim,      NoRollback,  GameFrame>;
#ifdef TNX_ENABLE_NETWORK
template class ::LogicThread<AuthoritySim, NoRollback,  GameFrame>;
template class ::LogicThread<OwnerSim, NoRollback, GameFrame>;
#endif
#ifdef TNX_ENABLE_ROLLBACK
template class ::LogicThread<SoloSim, RollbackSim, GameFrame>;
#ifdef TNX_ENABLE_NETWORK
template class ::LogicThread<AuthoritySim, RollbackSim, GameFrame>;
template class ::LogicThread<OwnerSim, RollbackSim, GameFrame>;
#endif
#endif
