#include "LogicThread.h"
#include "FramePacket.h"
#include "Registry.h"
#include "EngineConfig.h"
#include "Input.h"
#include "Profiler.h"
#include "Logger.h"
#include "TemporalComponentCache.h"
#include <SDL3/SDL.h>
#include <cmath>

#include "ThreadPinning.h"
#include "TrinyxEngine.h"
#include "JoltPhysics.h"

void LogicThread::Initialize(Registry* registry, const EngineConfig* config, JoltPhysics* physics,
							 InputBuffer* input, int windowWidth, int windowHeight)
{
	RegistryPtr    = registry;
	ConfigPtr      = config;
	PhysicsPtr     = physics;
	Input          = input;
	TemporalCache  = registry->GetTemporalCache();
	WindowWidth    = windowWidth;
	WindowHeight   = windowHeight;
	PhysicsDivizor = config->PhysicsUpdateInterval;

	LastCompletedFrame.store(0, std::memory_order_release);

	LOG_INFO("[LogicThread] Initialized");
}

void LogicThread::Start()
{
	bIsRunning.store(true, std::memory_order_release);
	Thread = std::thread(&LogicThread::ThreadMain, this);
	TrinyxThreading::PinThread(Thread);
	LOG_INFO("[LogicThread] Started");
}

void LogicThread::Stop()
{
	bIsRunning.store(false, std::memory_order_release);
	LOG_INFO("[LogicThread] Stop requested");
}

void LogicThread::Join()
{
	if (Thread.joinable())
	{
		Thread.join();
		LOG_INFO("[LogicThread] Joined");
	}
}

double LogicThread::GetFixedAlpha() const
{
	return Accumulator.load(std::memory_order_relaxed) / ConfigPtr->GetFixedStepTime();
}

void LogicThread::ThreadMain()
{
	const uint64_t perfFrequency = SDL_GetPerformanceFrequency();
	uint64_t lastCounter         = SDL_GetPerformanceCounter();

	// Cache config values
	const double fixedStepTime = ConfigPtr->GetFixedStepTime();

	// Safety caps
	constexpr double kMaxDt              = 0.25;
	constexpr double kMaxAccumulatedTime = 0.25;
	constexpr int kMaxPhysSubSteps       = 8;

	while (!TrinyxEngine::Get().GetJobsInitialized())
	{
	}

	// Stamp our thread ID so SpawnSync knows who the Logic thread is
	TrinyxEngine::Get().GetSpawner().SetLogicThreadId(std::this_thread::get_id());

	while (bIsRunning.load(std::memory_order_acquire))
	{
		TNX_ZONE_NC("Logic Frame", TNX_COLOR_LOGIC);

		// Sync point for deferred spawns — if any thread is waiting to
		// spawn entities, freeze here and let it write to the current frame.
		TrinyxEngine::Get().GetSpawner().SyncPoint();

		// Measure delta time
		const uint64_t frameStartCounter = SDL_GetPerformanceCounter();
		uint64_t counterElapsed          = frameStartCounter - lastCounter;
		lastCounter                      = frameStartCounter;

		double dt = static_cast<double>(counterElapsed) / static_cast<double>(perfFrequency);

		// Spiral of death cap
		if (dt > kMaxDt) dt = kMaxDt;

#if TNX_ENABLE_EDITOR
		// When paused: still process input (camera) and publish frames
		// so the scene is visible, but skip all simulation.
		if (bSimPaused.load(std::memory_order_acquire))
		{
			ProcessInput(dt);
			PublishCompletedFrame();
			RegistryPtr->PropagateFrame(FrameNumber++);

			if (ConfigPtr->TargetFPS > 0) WaitForTiming(frameStartCounter, perfFrequency);
			continue;
		}
#endif

		double acc = Accumulator.load(std::memory_order_relaxed) + dt;
		if (acc > kMaxAccumulatedTime) acc = kMaxAccumulatedTime;
		Accumulator.store(acc, std::memory_order_relaxed);

		// Fixed update loop with substepping
		if (Accumulator.load(std::memory_order_relaxed) >= fixedStepTime) [[unlikely]]
		{
			TNX_ZONE_NC("Physics Loop", TNX_COLOR_LOGIC);

			int steps = 0;
			while (Accumulator.load(std::memory_order_relaxed) >= fixedStepTime && steps < kMaxPhysSubSteps)
			{
				// FPS tracking
				FpsFixedCount++;
				FpsFixedTimer += fixedStepTime;

				ProcessInput(fixedStepTime);
				PrePhysics(fixedStepTime);

				if (FrameNumber % PhysicsDivizor == 0) [[unlikely]]
				{
					TrinyxJobs::Dispatch([this, fixedStepTime](uint32_t)
					{
						PhysicsPtr->Step(static_cast<float>(fixedStepTime * PhysicsDivizor));
					}, PhysicsPtr->GetJoltPhysCounter(), TrinyxJobs::Queue::Physics);
				}

				// Flush changes, then pull new transforms. This order means we remove orphans before pulling
				if (FrameNumber % PhysicsDivizor == PhysicsDivizor - 1) [[unlikely]]
				{
					PhysicsPtr->FlushPendingBodies(RegistryPtr);
					PhysicsPtr->PullActiveTransforms(RegistryPtr);
				}

				PostPhysics(fixedStepTime);
				Accumulator.store(Accumulator.load(std::memory_order_relaxed) - fixedStepTime,
								  std::memory_order_relaxed);
				++steps;
				SimulationTime += fixedStepTime;

				// Publish completed frame to RenderThread
				PublishCompletedFrame();
				RegistryPtr->PropagateFrame(FrameNumber++);
			}
			TrackFPS();
			continue; // don't do the scalar update immediately.
		}

		// Scalar update only if we're pretty sure we have time.
		if (fixedStepTime - dt > Accumulator.load(std::memory_order_relaxed)) [[likely]]
		{
			ScalarUpdate(dt);
			TrackFPS();
		}

		// Frame limiter (if MaxFPS is set in config)
		if (ConfigPtr->TargetFPS > 0)
		{
			WaitForTiming(frameStartCounter, perfFrequency);
		}
	}
}

void LogicThread::ProcessInput(double dt)
{
	Input->Swap();

	// ── Mouse look ───────────────────────────────────────────────────────
	CamYaw   += Input->GetMouseDX() * CamMouseSens;
	CamPitch -= Input->GetMouseDY() * CamMouseSens;

	// Clamp pitch to avoid gimbal lock at poles
	constexpr float kMaxPitch = 1.5533f; // ~89 degrees
	if (CamPitch > kMaxPitch) CamPitch = kMaxPitch;
	if (CamPitch < -kMaxPitch) CamPitch = -kMaxPitch;

	// ── WASD movement (camera-relative, flying) ──────────────────────────
	float sinYaw   = std::sin(CamYaw);
	float cosYaw   = std::cos(CamYaw);
	float sinPitch = std::sin(CamPitch);
	float cosPitch = std::cos(CamPitch);

	// Forward = full camera direction including pitch
	Vector3 forward{sinYaw * cosPitch, sinPitch, -cosYaw * cosPitch};
	Vector3 right{cosYaw, 0.0f, sinYaw};
	Vector3 up{0.0f, 1.0f, 0.0f};

	Vector3 moveDir{0.0f, 0.0f, 0.0f};

	if (Input->IsActionDown(Action::MoveForward)) moveDir = moveDir + forward;
	if (Input->IsActionDown(Action::MoveBackward)) moveDir = moveDir - forward;
	if (Input->IsActionDown(Action::MoveRight)) moveDir = moveDir + right;
	if (Input->IsActionDown(Action::MoveLeft)) moveDir = moveDir - right;
	if (Input->IsActionDown(Action::MoveUp)) moveDir = moveDir + up;
	if (Input->IsActionDown(Action::MoveDown)) moveDir = moveDir - up;

	// Normalize to prevent diagonal speed boost, then scale
	float moveLen = moveDir.Length();
	if (moveLen > 0.001f)
	{
		CamPos = CamPos + moveDir * (static_cast<float>(dt) * CamMoveSpeed / moveLen);
	}
}

void LogicThread::PublishCompletedFrame()
{
	TNX_ZONE_N("Logic_PublishFrame");

	// Get the frame header we just wrote to
	TemporalFrameHeader* header = TemporalCache->GetFrameHeader();

	// Fill frame metadata
	header->FrameNumber            = FrameNumber;
	header->ActiveEntityCount      = static_cast<uint32_t>(RegistryPtr->GetTotalEntityCount());
	header->TotalAllocatedEntities = static_cast<uint32_t>(RegistryPtr->GetTotalEntityCount());

	// Fill ViewState (basic perspective camera)
	float AspectRatio = static_cast<float>(WindowWidth) / static_cast<float>(WindowHeight);
	float Fov         = 60.0f * 3.14159f / 180.0f; // 60 degrees in radians
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

	float sinYaw   = std::sin(CamYaw);
	float cosYaw   = std::cos(CamYaw);
	float sinPitch = std::sin(CamPitch);
	float cosPitch = std::cos(CamPitch);

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
	float tx = -(camRight.x * CamPos.x + camRight.y * CamPos.y + camRight.z * CamPos.z);
	float ty = -(camUp.x * CamPos.x + camUp.y * CamPos.y + camUp.z * CamPos.z);
	float tz = (camForward.x * CamPos.x + camForward.y * CamPos.y + camForward.z * CamPos.z);

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

	header->CameraPosition = CamPos;
#if TNX_DEV_METRICS
	header->InputTimestamp = Input->GetSwapPerfCount();
#if TNX_DEV_METRICS_DETAILED
	if (Input->GetSwapPerfCount() != 0)
	{
		double bufferMs = static_cast<double>(Input->GetCurrentSwapTime() - Input->GetSwapPerfCount())
			/ static_cast<double>(SDL_GetPerformanceFrequency()) * 1000.0;
		LOG_DEBUG_F("[Latency] Buffer: %.2fms (input wait in swap buffer)", bufferMs);
	}
#endif
#endif

	// TODO: Fill SceneState (sun direction, color)
	header->SunDirection     = Vector3{0.0f, -1.0f, 0.0f};
	header->SunColor         = Vector3{1.0f, 1.0f, 1.0f};
	header->AmbientIntensity = 0.2f;

	// Publish frame number atomically - RenderThread can now read this frame
	LastCompletedFrame.store(FrameNumber, std::memory_order_release);
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

		constexpr double kSleepMarginSec = 0.002;

		if (remainingSec > kSleepMarginSec)
		{
			const double sleepSec = remainingSec - kSleepMarginSec;
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
		LOG_DEBUG_F("Logic FPS: %d | Frame: %.2fms", static_cast<int>(fps), static_cast<double>(ms));
		FpsFrameCount = 0;
		FpsTimer      = 0.0;
	}

	if (FpsFixedTimer >= 1.0) [[unlikely]]
	{
		float ffps = static_cast<float>(FpsFixedCount / FpsFixedTimer);
		float fms  = static_cast<float>((FpsFixedTimer / FpsFixedCount) * 1000.0);
		FixedFPS.store(ffps, std::memory_order_relaxed);
		FixedFrameMs.store(fms, std::memory_order_relaxed);
		LOG_DEBUG_F("Fixed FPS: %d | Frame: %.2fms", static_cast<int>(ffps), static_cast<double>(fms));
		FpsFixedCount = 0;
		FpsFixedTimer = 0.0;
	}
}