#include "LogicThread.h"
#include "FramePacket.h"
#include "Registry.h"
#include "EngineConfig.h"
#include "Profiler.h"
#include "Logger.h"
#include "TemporalComponentCache.h"
#include <SDL3/SDL.h>
#include <cmath>

#include "ThreadPinning.h"
#include "TrinyxEngine.h"

void LogicThread::Initialize(Registry* registry, const EngineConfig* config, int windowWidth, int windowHeight)
{
	RegistryPtr   = registry;
	ConfigPtr     = config;
	TemporalCache = registry->GetTemporalCache();
	WindowWidth   = windowWidth;
	WindowHeight  = windowHeight;

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

	while (bIsRunning.load(std::memory_order_acquire))
	{
		TNX_ZONE_NC("Logic Frame", TNX_COLOR_LOGIC);

		// Measure delta time
		const uint64_t frameStartCounter = SDL_GetPerformanceCounter();
		uint64_t counterElapsed          = frameStartCounter - lastCounter;
		lastCounter                      = frameStartCounter;

		double dt = static_cast<double>(counterElapsed) / static_cast<double>(perfFrequency);

		// Spiral of death cap
		if (dt > kMaxDt) dt = kMaxDt;

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

				PrePhysics(fixedStepTime);
				// insert Sim physics here
				PostPhysics(fixedStepTime);
				Accumulator.store(Accumulator.load(std::memory_order_relaxed) - fixedStepTime,
								  std::memory_order_relaxed);
				++steps;
				SimulationTime += dt;

				// Publish completed frame to RenderThread
				RegistryPtr->PropagateFrame(FrameNumber);
				PublishCompletedFrame();
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

void LogicThread::ProcessInput()
{
	// TODO: Future feature - swap input mailbox
	// CurrentInput = InputMailbox.exchange(&InputFrontBuffer, std::memory_order_acq_rel);
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
	float ZFar        = 1000.0f;

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

	// View matrix: identity — camera at world origin looking down -Z.
	// The slab is memset-zeroed so the Matrix4 constructor never runs;
	// we must set the diagonal explicitly every frame.
	auto& V = header->ViewMatrix;
	V.m[0]  = 1.0f;
	V.m[1]  = 0.0f;
	V.m[2]  = 0.0f;
	V.m[3]  = 0.0f; // col 0
	V.m[4]  = 0.0f;
	V.m[5]  = 1.0f;
	V.m[6]  = 0.0f;
	V.m[7]  = 0.0f; // col 1
	V.m[8]  = 0.0f;
	V.m[9]  = 0.0f;
	V.m[10] = 1.0f;
	V.m[11] = 0.0f; // col 2
	V.m[12] = 0.0f;
	V.m[13] = 0.0f;
	V.m[14] = 0.0f;
	V.m[15] = 1.0f; // col 3

	// Camera position at origin
	header->CameraPosition.x = 0.0f;
	header->CameraPosition.y = 0.0f;
	header->CameraPosition.z = 0.0f;

	// TODO: Fill SceneState (sun direction, color)
	header->SunDirection     = Vector3{0.0f, -1.0f, 0.0f};
	header->SunColor         = Vector3{1.0f, 1.0f, 1.0f};
	header->AmbientIntensity = 0.2f;

	// Publish frame number atomically - RenderThread can now read this frame
	LastCompletedFrame.store(FrameNumber++, std::memory_order_release);
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
		LOG_DEBUG_F("Logic FPS: %d | Frame: %.2fms",
					static_cast<int>(FpsFrameCount / FpsTimer),
					(FpsTimer / FpsFrameCount) * 1000.0);
		FpsFrameCount = 0;
		FpsTimer      = 0.0;
	}

	if (FpsFixedTimer >= 1.0) [[unlikely]]
	{
		LOG_DEBUG_F("Fixed FPS: %d | Frame: %.2fms",
					static_cast<int>(FpsFixedCount / FpsFixedTimer),
					(FpsFixedTimer / FpsFixedCount) * 1000.0);
		FpsFixedCount = 0;
		FpsFixedTimer = 0.0;
	}
}
