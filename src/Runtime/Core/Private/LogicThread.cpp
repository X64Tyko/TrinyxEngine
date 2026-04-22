#include "LogicThread.h"
#include "ConstructRegistry.h"
#include "Registry.h"
#include "EngineConfig.h"
#include "Input.h"
#include "Profiler.h"
#include "Logger.h"
#include "TemporalComponentCache.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>

#include "CameraManager.h"
#include "ThreadPinning.h"
#include "JoltPhysics.h"

#ifdef TNX_ENABLE_ROLLBACK
#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#endif

void LogicThread::Initialize(Registry* registry, const EngineConfig* config, JoltPhysics* physics,
							 InputBuffer* simInput, InputBuffer* vizInput,
							 TrinyxMPSCRing<NetInputFrame>* inputAccumRing,
							 const std::atomic<bool>* inputAccumEnabled,
							 TrinyxJobs::WorldQueueHandle worldQueue, const std::atomic<bool>* jobsInitialized,
							 int windowWidth, int windowHeight)
{
	RegistryPtr       = registry;
	ConfigPtr         = config;
	PhysicsPtr        = physics;
	SimInput          = simInput;
	VizInput          = vizInput;
	InputAccumRing    = inputAccumRing;
	InputAccumEnabled = inputAccumEnabled;
	WQHandle          = worldQueue;
	JobsInitPtr       = jobsInitialized;
	TemporalCache     = registry->GetTemporalCache();
	WindowWidth       = windowWidth;
	WindowHeight      = windowHeight;
	PhysicsDivizor    = config->PhysicsUpdateInterval;

	LastCompletedFrame.store(0, std::memory_order_release);

#ifdef TNX_ENABLE_ROLLBACK
	IncomingCorrections.Initialize(256);
	IncomingPredictedCorrections.Initialize(256);
#endif

	LOG_ENG_INFO("[LogicThread] Initialized");
}

void LogicThread::Start()
{
	bIsRunning.store(true, std::memory_order_release);
	Thread = std::thread(&LogicThread::ThreadMain, this);
	TrinyxThreading::PinThread(Thread);
	LOG_ENG_INFO("[LogicThread] Started");
}

void LogicThread::Stop()
{
	bIsRunning.store(false, std::memory_order_release);
	LOG_ENG_INFO("[LogicThread] Stop requested");
}

void LogicThread::Join()
{
	if (Thread.joinable())
	{
		Thread.join();
		LOG_ENG_INFO("[LogicThread] Joined");
	}
}

double LogicThread::GetFixedAlpha() const
{
	return (ConfigPtr->GetFixedStepTime() - Accumulator.load(std::memory_order_relaxed)) / ConfigPtr->GetFixedStepTime();
}

// TODO: We need the editor not to Propagate frames when paused so that we can rewind time for debugging
bool LogicThread::TickPause(const uint64_t perfFrequency, const uint64_t frameStartCounter, double dt)
{
	// When paused: still process input (camera) and publish frames
	// so the scene is visible, but skip all simulation.
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

#ifdef TNX_ENABLE_ROLLBACK
void LogicThread::ProcessRollback()
{
	// Drop server events that have aged past the temporal ring — they can never
	// be targeted by a rollback and would only consume memory.
	if (FrameNumber > TemporalCache->GetTotalFrameCount())
		RegistryPtr->PruneServerEvents(FrameNumber - TemporalCache->GetTotalFrameCount());
					
	if (bRollbackTestRequested.load(std::memory_order_acquire)
		&& FrameNumber > RollbackFrameCount + PhysicsDivizor)
	{
		bRollbackTestRequested.store(false, std::memory_order_relaxed);
		ExecuteRollbackTest();
	}
	
	if (bRollbackActive) return;
	
	// Drain any corrections that arrived from worker/net threads since last tick.
	{
		EntityTransformCorrection staged;
		while (IncomingCorrections.TryPop(staged))
			PendingCorrections.push_back(staged);
	}

	// Merge any pending spawn rollback request into the correction set.
	// The spawn rollback has no correction values to check — it just ensures
	// the entity exists in the ring from its birth frame onward.
	uint32_t spawnRollbackFrame = PendingRollbackFrame.exchange(UINT32_MAX, std::memory_order_acq_rel);

	// Drop corrections whose target frame predates the temporal ring. The ring
	// slot for that frame has been reused for a newer frame, so any comparison
	// would read stale data and produce a spurious divergence that can never be
	// resolved — the ring can't go back that far and neither can Jolt.
	{
		const uint32_t ringSize   = TemporalCache->GetTotalFrameCount();
		const uint32_t currentF   = TemporalCache->GetFrameHeader()->FrameNumber;
		const uint32_t oldestSlab = (currentF >= ringSize - 1) ? (currentF - (ringSize - 1)) : 0u;

		const auto stalePred = [oldestSlab](const EntityTransformCorrection& c)
		{
			return c.ClientFrame < oldestSlab;
		};

		auto staleBegin = std::remove_if(PendingCorrections.begin(),
										 PendingCorrections.end(), stalePred);
		if (staleBegin != PendingCorrections.end())
		{
			LOG_ENG_WARN_F("[Rollback] Discarding %zu stale correction(s) (frame < %u, ring depth=%u)",
						   std::distance(staleBegin, PendingCorrections.end()),
						   oldestSlab, ringSize);
			PendingCorrections.erase(staleBegin, PendingCorrections.end());
		}
	}

	uint32_t minFrame = spawnRollbackFrame;

	if (!PendingCorrections.empty())
	{
		for (const auto& c : PendingCorrections)
			if (c.ClientFrame < minFrame) minFrame = c.ClientFrame;
	}

	if (minFrame != UINT32_MAX)
	{
		// Clamp against the oldest Jolt snapshot we still have. Rolling back beyond
		// the ring triggers an expensive cold rebuild; cap it to what we can restore.
		if (PhysicsPtr)
		{
			const uint32_t oldestSnap = PhysicsPtr->GetOldestSnapshotFrame();
			if (oldestSnap != UINT32_MAX && minFrame < oldestSnap)
			{
				LOG_ENG_WARN_F("[Rollback] Clamping target frame %u → %u (oldest Jolt snapshot)",
							   minFrame, oldestSnap);
				minFrame = oldestSnap;
			}
		}

		ExecuteRollback(minFrame);
	}
}
#endif

void LogicThread::PhysicsLoop(const SimFloat fixedStepTime)
{
	PrePhysics(fixedStepTime);
	ScalarPrePhysicsBatch.Execute(fixedStepTime);

	if (FrameNumber % PhysicsDivizor == 0) [[unlikely]]
	{
		PhysicsPtr->FlushPendingBodies(RegistryPtr);
		PhysicsPtr->PushKinematicTransforms(RegistryPtr, fixedStepTime);
		ScalarPhysicsStepBatch.Execute(fixedStepTime * PhysicsDivizor);
		TrinyxJobs::Dispatch([this, fixedStepTime](uint32_t)
		{
			PhysicsPtr->Step(fixedStepTime * PhysicsDivizor);
		}, PhysicsPtr->GetJoltPhysCounter(), TrinyxJobs::Queue::Physics);
	}

	// Flush changes, then pull new transforms. This order means we remove orphans before pulling
	if (FrameNumber % PhysicsDivizor == PhysicsDivizor - 1) [[unlikely]]
	{
		PhysicsPtr->PullActiveTransforms(RegistryPtr);
#ifdef TNX_ENABLE_ROLLBACK
		PhysicsPtr->SaveSnapshot(FrameNumber);
#endif
	}

	PostPhysics(fixedStepTime);
	ScalarPostPhysicsBatch.Execute(fixedStepTime);

#ifdef TNX_ENABLE_ROLLBACK
	// Apply any server corrections that target exactly this frame.
	// These arrived for frames the client hadn't processed yet, so no rollback is needed —
	// the forward pass hasn't committed this frame's data yet when we reach here.
	{
		EntityTransformCorrection corr{};
		while (IncomingPredictedCorrections.TryPop(corr))
		{
			if (corr.ClientFrame > FrameNumber)
			{
				PendingPredictedCorrections.push(corr);
				continue;
			}
			RegistryPtr->CheckAndCorrectEntityTransform(corr);
		}

		while (PendingPredictedCorrections.size() > 0)
		{
			corr = PendingPredictedCorrections.front();
			PendingPredictedCorrections.pop();
			if (!IncomingPredictedCorrections.TryPush(corr))
			LOG_ENG_WARN_F("[Rollback] Discarding stale predicted correction (frame %u)", corr.ClientFrame);
		}
	}
#endif
}

bool LogicThread::FixedUpdate(const uint64_t perfFrequency, const SimFloat fixedStepTime, const int MaxPhysSubSteps, const uint64_t frameStartCounter)
{
	// Fixed update loop with substepping
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

			// FPS tracking
			FpsFixedCount++;
			FpsFixedTimer += fixedStepTime;

#ifdef TNX_ENABLE_ROLLBACK
			RecordFrameInput();
#endif
			
			PhysicsLoop(fixedStepTime);
			
			Accumulator.store(Accumulator.load(std::memory_order_relaxed) + fixedStepTime,
							  std::memory_order_relaxed);
			++steps;
			SimulationTime += fixedStepTime;

			// Publish completed frame to RenderThread
			ProcessVizInput(fixedStepTime);
#ifdef TNX_ENABLE_ROLLBACK
			RegistryPtr->ReplayServerEventsAt(FrameNumber);
#endif
			PublishCompletedFrame();
			RegistryPtr->PropagateFrame(FrameNumber++);

#ifdef TNX_ENABLE_ROLLBACK
			ProcessRollback();
#endif
		}
		
		TrackFPS();
		// When stalled, sleep for one tick so the NetThread can process incoming
		// InputFrame packets and advance LastReceivedFrame before we retry.
		if (bDidStall || ConfigPtr->TargetFPS > 0)
			WaitForTiming(frameStartCounter, perfFrequency);
		return true; // don't do the scalar update immediately.
	}
	return false;
}

void LogicThread::ThreadMain()
{
	const uint64_t perfFrequency = SDL_GetPerformanceFrequency();
	uint64_t lastCounter         = SDL_GetPerformanceCounter();

	// Cache config values — SimFloat is float by default, Fixed32 when TNX_DETERMINISTIC
	const SimFloat fixedStepTime = static_cast<SimFloat>(ConfigPtr->GetFixedStepTime());

	// Safety caps
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

		// Measure delta time
		const uint64_t frameStartCounter = SDL_GetPerformanceCounter();
		uint64_t counterElapsed          = frameStartCounter - lastCounter;
		lastCounter                      = frameStartCounter;

		double dt = static_cast<double>(counterElapsed) / static_cast<double>(perfFrequency);

		// Spiral of death cap
		if (dt > MaxDt) dt = MaxDt;

#if TNX_ENABLE_EDITOR
		if (TickPause(perfFrequency, frameStartCounter, dt)) continue;
#endif

		double acc = Accumulator.load(std::memory_order_relaxed) - dt;
		if (acc < MaxAccumulatedTime) acc = MaxAccumulatedTime;
		Accumulator.store(acc, std::memory_order_relaxed);

		if (FixedUpdate(perfFrequency, fixedStepTime, MaxPhysSubSteps, frameStartCounter)) continue;

		// Scalar update only if we're pretty sure we have time.
		if (dt > Accumulator.load(std::memory_order_relaxed)) [[likely]]
		{
			ScalarUpdate(static_cast<SimFloat>(dt));
			ScalarUpdateBatch.Execute(static_cast<SimFloat>(dt));
			TrackFPS();
		}

		// Frame limiter (if MaxFPS is set in config)
		if (ConfigPtr->TargetFPS > 0)
		{
			WaitForTiming(frameStartCounter, perfFrequency);
		}
	}
}

bool LogicThread::ProcessSimInput(SimFloat dt)
{
	SimInput->Swap();

	// Snapshot this frame's input for the net thread (client-side only).
	// Runs immediately after Swap so frame tag is exact — FRONT holds the new data.
	if (InputAccumRing && InputAccumEnabled && InputAccumEnabled->load(std::memory_order_acquire))
	{
		NetInputFrame snap{};
		snap.Frame = FrameNumber;
		SimInput->SnapshotKeyState(snap.State.KeyState, sizeof(snap.State.KeyState));
		snap.State.MouseDX      = SimInput->GetMouseDX();
		snap.State.MouseDY      = SimInput->GetMouseDY();
		snap.State.MouseButtons = SimInput->GetMouseButtonMask();

		const uint16_t evCount = SimInput->GetEventCount();
		snap.EventCount = static_cast<uint8_t>(evCount < 8 ? evCount : 8);
		for (uint8_t i = 0; i < snap.EventCount; ++i)
		{
			InputData e = SimInput->ReadEvent();
			snap.Events[i].Key          = static_cast<uint32_t>(e.Key);
			snap.Events[i].FrameUSOffset = e.FrameUSOffset;
			snap.Events[i].Pressed      = e.Pressed;
			snap.Events[i]._Pad         = 0;
		}

		// Overwrite the oldest pre-connection frame if the ring is full so the consumer
		// always sees the most recent Capacity frames when it drains on connect.
		InputAccumRing->OverwritePush(snap);
	}

	// Server-side: pull each connected player's input from the PlayerInputLog for this frame.
	// Returns true if any player's input window hasn't arrived yet — caller must hold FrameNumber.
	if (PlayerInputInjector) return PlayerInputInjector(FrameNumber);
	return false;
}

void LogicThread::ProcessVizInput(SimFloat dt)
{
	VizInput->Swap();

	if (LocalCameraManager) LocalCameraManager->Tick(dt);

	// Free-fly fallback — skip when CameraManager has active layers.
	if (LocalCameraManager && LocalCameraManager->HasActiveLayers()) return;

	CamYaw   += VizInput->GetMouseDX() * CamMouseSens;
	CamPitch -= VizInput->GetMouseDY() * CamMouseSens;

	constexpr float MaxPitch = 1.5533f; // ~89 degrees
	if (CamPitch > MaxPitch) CamPitch = MaxPitch;
	if (CamPitch < -MaxPitch) CamPitch = -MaxPitch;

	float sinYaw   = std::sin(CamYaw);
	float cosYaw   = std::cos(CamYaw);
	float sinPitch = std::sin(CamPitch);
	float cosPitch = std::cos(CamPitch);

	Vector3 forward{sinYaw * cosPitch, sinPitch, -cosYaw * cosPitch};
	Vector3 right{cosYaw, 0.0f, sinYaw};
	Vector3 up{0.0f, 1.0f, 0.0f};

	Vector3 moveDir{0.0f, 0.0f, 0.0f};

	if (VizInput->IsActionDown(Action::MoveForward)) moveDir = moveDir + forward;
	if (VizInput->IsActionDown(Action::MoveBackward)) moveDir = moveDir - forward;
	if (VizInput->IsActionDown(Action::MoveRight)) moveDir = moveDir + right;
	if (VizInput->IsActionDown(Action::MoveLeft)) moveDir = moveDir - right;
	if (VizInput->IsActionDown(Action::MoveUp)) moveDir = moveDir + up;
	if (VizInput->IsActionDown(Action::MoveDown)) moveDir = moveDir - up;

	float moveLen = moveDir.Length();
	if (moveLen > 0.001f)
		CamPos = CamPos + moveDir * (static_cast<float>(dt) * CamMoveSpeed / moveLen);
}

void LogicThread::PublishCompletedFrame()
{
	TNX_ZONE_N("Logic_PublishFrame");

	TemporalFrameHeader* header = TemporalCache->GetFrameHeader();

	header->FrameNumber            = FrameNumber;
	header->ActiveEntityCount      = static_cast<uint32_t>(RegistryPtr->GetTotalEntityCount());
	header->TotalAllocatedEntities = static_cast<uint32_t>(RegistryPtr->GetTotalEntityCount());

	float activeFOV   = 60.0f;
	float activeYaw   = CamYaw, activePitch = CamPitch;
	Vector3 activePos = CamPos;

	if (LocalCameraManager)
	{
		WorldCameraState cs = LocalCameraManager->Resolve();
		if (cs.Valid)
		{
			activePos   = cs.Position;
			activeYaw   = cs.Yaw;
			activePitch = cs.Pitch;
			activeFOV   = cs.FOV;
		}
	}

	// Fill ViewState (basic perspective camera)
	float AspectRatio = static_cast<float>(WindowWidth) / static_cast<float>(WindowHeight);
	float Fov         = activeFOV * 3.14159f / 180.0f;
	float ZNear       = 0.1f;
	float ZFar        = 5000.0f;

	// Vulkan perspective matrix (column-major, RH coordinates, depth [0,1]).
	//   - Camera looks down -Z; entities at negative Z are visible.
	//   - Y is negated (m[5] = -F) to match Vulkan's Y-down NDC convention.
	//   - Depth: z_view=-ZNear → NDC.z=0,  z_view=-ZFar → NDC.z=1.
	//
	// Column layout m[col*4+row]:
	//   col 0: [ F/aspect,  0,  0,  0 ]
	//   col 1: [        0, -F,  0,  0 ]   <- Y flip
	//   col 2: [        0,  0,  a, -1 ]   <- perspective divide (w = -z_view)
	//   col 3: [        0,  0,  b,  0 ]   <- depth translation
	//
	// where a = -ZFar/(ZFar-ZNear),  b = -ZFar*ZNear/(ZFar-ZNear)
	float F  = 1.0f / std::tan(Fov * 0.5f);
	float dz = ZNear - ZFar; // negative (ZFar > ZNear)
	auto& P  = header->ProjectionMatrix;

	P.m[0]  = F / AspectRatio;
	P.m[1]  = 0.0f;
	P.m[2]  = 0.0f;
	P.m[3]  = 0.0f; // col 0
	P.m[4]  = 0.0f;
	P.m[5]  = -F;
	P.m[6]  = 0.0f;
	P.m[7]  = 0.0f; // col 1
	P.m[8]  = 0.0f;
	P.m[9]  = 0.0f;
	P.m[10] = ZFar / dz; // a = -ZFar/(ZFar-ZNear)
	P.m[11] = -1.0f;     // perspective divide: clip.w = -z_view
	P.m[12] = 0.0f;
	P.m[13] = 0.0f;
	P.m[14] = (ZFar * ZNear) / dz; // b = -ZFar*ZNear/(ZFar-ZNear)
	P.m[15] = 0.0f;

	// View matrix from camera yaw/pitch/position.
	// RH coordinate system: camera looks down -Z at yaw=0, Y is up.
	// The slab is memset-zeroed so Matrix4 constructor never runs;
	// we must write every element explicitly.
	//
	// Basis vectors:
	//   forward = (sin(yaw)*cos(pitch), sin(pitch), -cos(yaw)*cos(pitch))
	//   right   = (cos(yaw), 0, sin(yaw))
	//   up      = cross(right, forward)
	//
	// View matrix rows = camera axes (transposed rotation), col 3 = -dot(axis, pos).
	// Column-major layout: m[col*4 + row].

	float sinYaw   = std::sin(activeYaw);
	float cosYaw   = std::cos(activeYaw);
	float sinPitch = std::sin(activePitch);
	float cosPitch = std::cos(activePitch);

	// Camera basis
	Vector3 camForward{sinYaw * cosPitch, sinPitch, -cosYaw * cosPitch};
	Vector3 camRight{cosYaw, 0.0f, sinYaw};
	// up = cross(right, forward)
	Vector3 camUp{
		camRight.y * camForward.z - camRight.z * camForward.y,
		camRight.z * camForward.x - camRight.x * camForward.z,
		camRight.x * camForward.y - camRight.y * camForward.x
	};

	// Translation = -dot(axis, position)
	float tx = -(camRight.x * activePos.x + camRight.y * activePos.y + camRight.z * activePos.z);
	float ty = -(camUp.x * activePos.x + camUp.y * activePos.y + camUp.z * activePos.z);
	float tz = (camForward.x * activePos.x + camForward.y * activePos.y + camForward.z * activePos.z);

	auto& V = header->ViewMatrix;
	V.m[0]  = camRight.x;
	V.m[1]  = camUp.x;
	V.m[2]  = -camForward.x;
	V.m[3]  = 0.0f;
	V.m[4]  = camRight.y;
	V.m[5]  = camUp.y;
	V.m[6]  = -camForward.y;
	V.m[7]  = 0.0f;
	V.m[8]  = camRight.z;
	V.m[9]  = camUp.z;
	V.m[10] = -camForward.z;
	V.m[11] = 0.0f;
	V.m[12] = tx;
	V.m[13] = ty;
	V.m[14] = tz;
	V.m[15] = 1.0f;

	header->CameraPosition = activePos;
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

	// TODO: Fill SceneState (sun direction, color)
	header->SunDirection     = Vector3{0.0f, -1.0f, 0.0f};
	header->SunColor         = Vector3{1.0f, 1.0f, 1.0f};
	header->AmbientIntensity = 0.2f;

	// Publish frame number atomically - RenderThread can now read this frame
	LastCompletedFrame.store(FrameNumber, std::memory_order_release);
	RegistryPtr->LastPublishedFrame = FrameNumber;
}

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
			// Busy wait
		}
	}
}

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

// ─────────────────────────────────────────────────────────────────────────────
// Rollback determinism test
// ─────────────────────────────────────────────────────────────────────────────
#ifdef TNX_ENABLE_ROLLBACK

void LogicThread::RecordFrameInput()
{
	TemporalFrameHeader* header = TemporalCache->GetFrameHeader();
	std::memcpy(header->InputKeyState, SimInput->KeyState[SimInput->ReadSlot], 64);
	header->InputMouseDX = SimInput->GetMouseDX();
	header->InputMouseDY = SimInput->GetMouseDY();
}

void LogicThread::InjectFrameInput(uint32_t frameNum)
{
	TemporalFrameHeader* header = TemporalCache->GetFrameHeader(frameNum);
	uint8_t readSlot            = SimInput->ReadSlot;
	std::memcpy(SimInput->KeyState[readSlot], header->InputKeyState, 64);
	SimInput->MouseDX[readSlot]    = header->InputMouseDX;
	SimInput->MouseDY[readSlot]    = header->InputMouseDY;
	SimInput->EventCount[readSlot] = 0;
	SimInput->ReadCursor           = 0;
}

void LogicThread::EnqueueCorrections(std::vector<EntityTransformCorrection> corrections,
									 [[maybe_unused]] uint32_t earliestClientFrame)
{
	// Push to the lock-free ring buffer so worker/net threads don't race with the
	// logic thread's PendingCorrections vector. The logic thread drains IncomingCorrections
	// into PendingCorrections at the start of each rollback check.
	for (auto& c : corrections)
	{
		if (!IncomingCorrections.TryPush(std::move(c)))
			LOG_ENG_WARN("[Rollback] IncomingCorrections full — correction dropped");
	}
}

void LogicThread::EnqueuePredictedCorrections(std::vector<EntityTransformCorrection> corrections)
{
	for (auto& c : corrections)
	{
		if (!IncomingPredictedCorrections.TryPush(std::move(c)))
			LOG_ENG_WARN("[Rollback] IncomingPredictedCorrections full — correction dropped");
	}
}

void LogicThread::EnqueueSpawnRollback(uint32_t clientFrame)
{
	// Atomically update PendingRollbackFrame to be the minimum of the current pending
	// value and the requested frame (oldest wins so we cover all spawns in one pass).
	uint32_t current = PendingRollbackFrame.load(std::memory_order_relaxed);
	while (clientFrame < current)
	{
		if (PendingRollbackFrame.compare_exchange_weak(current, clientFrame,
			std::memory_order_release, std::memory_order_relaxed))
			break;
	}
}

void LogicThread::ExecuteRollback(uint32_t targetFrame)
{
	TNX_ZONE_N("Rollback");

	bRollbackActive = true;

	const uint32_t T           = FrameNumber - 1;
	const uint32_t frameCount  = TemporalCache->GetTotalFrameCount();
	const double fixedStepTime = ConfigPtr->GetFixedStepTime();

	// Align to the physics snapshot boundary: largest frame F ≤ targetFrame where Jolt flushed.
	// Snapshot frames are at D-1, 2D-1, ... so this is (targetFrame / D) * D - 1.
	const uint32_t alignedTarget = (targetFrame / PhysicsDivizor) * PhysicsDivizor - 1;

	if (alignedTarget >= T)
	{
		LOG_ENG_WARN_F("[Rollback] Target frame %u (aligned from %u) is at or beyond current frame %u — skipping",
					   alignedTarget, targetFrame, T);
		bRollbackActive = false;
		PendingCorrections.clear();
		return;
	}

	const uint32_t totalResimFrames = (T - alignedTarget) + 1;

	LOG_ENG_INFO_F("[%s Rollback] Rewind to frame %u (aligned from %u), resim %u frames to frame %u",
				   EngineModeNames[(uint8_t)ConfigPtr->Mode], alignedTarget, targetFrame, totalResimFrames, T);

	// ── Rewind ─────────────────────────────────────────────────────────────
	{
		TNX_ZONE_N("Rollback_Rewind");

		// Resims don't nest and PropagateFrame runs inside the resim loop, so the
		// ring slot invariant (frame F → slot F % N) holds — no scan needed.
		TemporalCache->SetActiveWriteFrame(alignedTarget % frameCount);

		TrinyxJobs::WaitForCounter(PhysicsPtr->GetJoltPhysCounter(), TrinyxJobs::Queue::Logic);

		if (!PhysicsPtr->RestoreSnapshot(alignedTarget))
		{
			LOG_ENG_WARN("[Rollback] Snapshot not found, falling back to rebuild-from-slab");
			PhysicsPtr->ResetAllBodies();
			PhysicsPtr->FlushPendingBodies(RegistryPtr);
		}

		FrameNumber = alignedTarget;
		RegistryPtr->ReplayServerEventsAt(FrameNumber);
		PhysicsPtr->SaveSnapshot(alignedTarget);
		RegistryPtr->PropagateFrame(FrameNumber++);

		SimulationTime = FrameNumber * fixedStepTime;
	}

	LOG_ENG_INFO_F("[Rollback] Jolt restored, starting resim from frame %u", FrameNumber);

	// ── Resimulate ─────────────────────────────────────────────────────────
	{
		TNX_ZONE_N("Rollback_Resim");

		for (uint32_t i = 0; i < totalResimFrames; ++i)
		{

			InjectFrameInput(FrameNumber); // restore world SimInput from frame header
			// Replay per-player net input so constructs reading soul->GetSimInput() get the
			// correct real (or re-predicted) state for this frame, not the stale forward-pass buffer.
			if (PlayerInputInjector) PlayerInputInjector(FrameNumber);
			PhysicsLoop(fixedStepTime);

			// Check pending server corrections for this frame. If the resimmed value still
			// diverges from the server-authoritative position, overwrite it now.
			for (auto it = PendingCorrections.begin(); it != PendingCorrections.end();)
			{
				if (it->ClientFrame != FrameNumber)
				{
					++it;
					continue;
				}
				if (RegistryPtr->CheckAndCorrectEntityTransform(*it))
				{
					LOG_ENG_INFO_F("[Rollback] Correction applied at frame %u for netHandle=%u",
								   FrameNumber, it->NetHandle);
				}
				it = PendingCorrections.erase(it);
			}

			// Replay server-driven events (spawns, sweeps).
			// Values come from completed frames
			RegistryPtr->ReplayServerEventsAt(FrameNumber);

			RegistryPtr->PropagateFrame(FrameNumber++);
		}

		LOG_ENG_INFO_F("[Rollback] Resimulation complete, frame %u", FrameNumber);
	}

	// Discard only corrections that fall before the resim window — corrections for the
	// current frame or future frames must survive for the next rollback pass to apply them.
	PendingCorrections.erase(
		std::remove_if(PendingCorrections.begin(), PendingCorrections.end(),
			[this](const EntityTransformCorrection& c) { return c.ClientFrame < FrameNumber; }),
		PendingCorrections.end());
	bRollbackActive = false;
}

void LogicThread::ExecuteRollbackTest()
{
	TNX_ZONE_N("Rollback_Test");

	const uint32_t T              = FrameNumber - 1;
	const uint32_t rollbackTarget = T - RollbackFrameCount;
	[[maybe_unused]] const double fixedStepTime = ConfigPtr->GetFixedStepTime();

#ifdef TNX_TESTING
	// ── Save ground truth (test harness only) ─────────────────────────────
	const size_t fieldDataSize             = TemporalCache->GetFrameStride() - sizeof(TemporalFrameHeader);
	const uint32_t groundTruthSlot         = TemporalCache->GetActiveReadFrame();
	TemporalFrameHeader* groundTruthHeader = TemporalCache->GetFrameHeader(groundTruthSlot);
	uint8_t* groundTruthFieldData          = reinterpret_cast<uint8_t*>(groundTruthHeader) + sizeof(TemporalFrameHeader);

	ComponentCacheBase* volatileCache = RegistryPtr->GetVolatileCache();
	const size_t temporalSlabSize     = TemporalCache->GetTotalSlabSize();
	const size_t volatileSlabSize     = volatileCache->GetTotalSlabSize();

	{
		TNX_ZONE_N("Rollback_Backup");

		GroundTruthBackup.resize(fieldDataSize);
		std::memcpy(GroundTruthBackup.data(), groundTruthFieldData, fieldDataSize);

		TemporalSlabBackup.resize(temporalSlabSize);
		VolatileSlabBackup.resize(volatileSlabSize);
		std::memcpy(TemporalSlabBackup.data(), TemporalCache->GetSlabPtr(), temporalSlabSize);
		std::memcpy(VolatileSlabBackup.data(), volatileCache->GetSlabPtr(), volatileSlabSize);
	}

	const uint32_t savedTemporalWrite = TemporalCache->GetActiveWriteFrame();
	const uint32_t savedTemporalRead  = TemporalCache->GetActiveReadFrame();
	const uint32_t savedVolatileWrite = volatileCache->GetActiveWriteFrame();
	const uint32_t savedVolatileRead  = volatileCache->GetActiveReadFrame();

	JPH::StateRecorderImpl savedJolt;
	{
		TNX_ZONE_N("Rollback_SaveJolt");
		PhysicsPtr->GetPhysicsSystem()->SaveState(savedJolt, JPH::EStateRecorderState::All);
	}

	const uint32_t savedFrameNumber = FrameNumber;
	const double savedSimTime       = SimulationTime;
#endif // TNX_TESTING

	// ── Rewind + Resimulate ────────────────────────────────────────────────
	ExecuteRollback(rollbackTarget);

#ifdef TNX_TESTING
	// ── Compare (test harness only) ────────────────────────────────────────
	{
		TNX_ZONE_N("Rollback_Compare");

		const uint32_t resimSlot         = TemporalCache->GetActiveReadFrame();
		TemporalFrameHeader* resimHeader = TemporalCache->GetFrameHeader(resimSlot);
		uint8_t* resimFieldData          = reinterpret_cast<uint8_t*>(resimHeader) + sizeof(TemporalFrameHeader);

		int cmp = std::memcmp(GroundTruthBackup.data(), resimFieldData, fieldDataSize);
		if (cmp == 0)
		{
			LOG_ENG_INFO_F("[Rollback] PASSED — byte-perfect determinism (%zu bytes, %u frames resimulated)",
						   fieldDataSize, RollbackFrameCount);
		}
		else
		{
			LOG_ENG_WARN_F("[Rollback] FAILED — divergence detected (%u frames resimulated)", RollbackFrameCount);

			auto fieldInfos = TemporalCache->GetValidFieldInfos();
			for (const auto& info : fieldInfos)
			{
				if (info.CurrentUsed == 0) continue;

				const uint8_t* truthField = GroundTruthBackup.data() + info.OffsetInFrame;
				const uint8_t* resimField = resimFieldData + info.OffsetInFrame;

				int fieldCmp = std::memcmp(truthField, resimField, info.CurrentUsed);
				if (fieldCmp != 0)
				{
					size_t firstDiff = 0;
					for (size_t b = 0; b < info.CurrentUsed; ++b)
					{
						if (truthField[b] != resimField[b])
						{
							firstDiff = b;
							break;
						}
					}

					size_t entityIdx   = firstDiff / info.FieldSize;
					size_t byteInField = firstDiff % info.FieldSize;

					size_t divergentBytes = 0;
					for (size_t b = 0; b < info.CurrentUsed; ++b) divergentBytes += (truthField[b] != resimField[b]);

					LOG_ENG_WARN_F("  DIVERGE: %s (comp=%u field=%zu) entity=%zu+%zu "
								   "divergent=%zu/%zu (%.2f%%)",
							   info.FieldName, info.CompType, info.FieldIndex,
							   entityIdx, byteInField,
							   divergentBytes, info.CurrentUsed,
							   100.0 * static_cast<double>(divergentBytes) / static_cast<double>(info.CurrentUsed));
				}
			}
		}

		// Compare Jolt state
		JPH::StateRecorderImpl resimJolt;
		PhysicsPtr->GetPhysicsSystem()->SaveState(resimJolt, JPH::EStateRecorderState::All);
		std::string resimJoltData = resimJolt.GetData();
		std::string savedJoltData = savedJolt.GetData();

		if (resimJoltData == savedJoltData)
		{
			LOG_ENG_INFO_F("[Rollback] Jolt physics: MATCH (%zu bytes)", resimJoltData.size());
		}
		else
		{
			LOG_ENG_WARN_F("[Rollback] Jolt physics: DIVERGED (truth=%zu bytes, resim=%zu bytes)",
						   savedJoltData.size(), resimJoltData.size());
			size_t minLen = std::min(savedJoltData.size(), resimJoltData.size());
			for (size_t i = 0; i < minLen; ++i)
			{
				if (savedJoltData[i] != resimJoltData[i])
				{
					LOG_ENG_WARN_F("  First Jolt divergence at byte %zu: truth=0x%02x resim=0x%02x",
								   i, static_cast<uint8_t>(savedJoltData[i]), static_cast<uint8_t>(resimJoltData[i]));
					break;
				}
			}
		}
	}

	// ── Restore (test harness only) ────────────────────────────────────────
	{
		TNX_ZONE_N("Rollback_Restore");

		std::memcpy(TemporalCache->GetSlabPtr(), TemporalSlabBackup.data(), temporalSlabSize);
		std::memcpy(volatileCache->GetSlabPtr(), VolatileSlabBackup.data(), volatileSlabSize);

		TemporalCache->SetActiveWriteFrame(savedTemporalWrite);
		TemporalCache->SetLastWrittenFrame(savedTemporalRead);
		volatileCache->SetActiveWriteFrame(savedVolatileWrite);
		volatileCache->SetLastWrittenFrame(savedVolatileRead);

		savedJolt.Rewind();
		PhysicsPtr->GetPhysicsSystem()->RestoreState(savedJolt);

		FrameNumber    = savedFrameNumber;
		SimulationTime = savedSimTime;
	}

	LOG_ENG_INFO("[Rollback] State restored, simulation continuing.");
#endif // TNX_TESTING
}

#endif // TNX_ENABLE_ROLLBACK
