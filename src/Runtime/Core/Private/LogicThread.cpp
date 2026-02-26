#include "LogicThread.h"
#include "FramePacket.h"
#include "Registry.h"
#include "EngineConfig.h"
#include "Profiler.h"
#include "Logger.h"
#include "TemporalComponentCache.h"
#include <SDL3/SDL.h>
#include <cmath>

void LogicThread::Initialize(Registry* registry, const EngineConfig* config, int windowWidth, int windowHeight)
{
    RegistryPtr = registry;
    ConfigPtr = config;
    TemporalCache = registry->GetTemporalCache();
    WindowWidth = windowWidth;
    WindowHeight = windowHeight;

    LastCompletedFrame.store(0, std::memory_order_release);

    LOG_INFO("[LogicThread] Initialized");
}

void LogicThread::Start()
{
    bIsRunning.store(true, std::memory_order_release);
    Thread = std::thread(&LogicThread::ThreadMain, this);
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
    uint64_t lastCounter = SDL_GetPerformanceCounter();

    // Cache config values
    const double fixedStepTime = ConfigPtr->GetFixedStepTime();

    // Safety caps
    constexpr double kMaxDt = 0.25;
    constexpr double kMaxAccumulatedTime = 0.25;
    constexpr int kMaxPhysSubSteps = 8;

    while (bIsRunning.load(std::memory_order_acquire))
    {
        STRIGID_ZONE_C(STRIGID_COLOR_LOGIC);

        // Measure delta time
        const uint64_t frameStartCounter = SDL_GetPerformanceCounter();
        uint64_t counterElapsed = frameStartCounter - lastCounter;
        lastCounter = frameStartCounter;

        double dt = static_cast<double>(counterElapsed) / static_cast<double>(perfFrequency);

        // FPS tracking
        FpsFrameCount++;
        FpsTimer += dt;

        if (FpsTimer >= 1.0)
        {
            double fps = FpsFrameCount / FpsTimer;
            double ms = (FpsTimer / FpsFrameCount) * 1000.0;

            LOG_DEBUG_F("Logic FPS: %d | Frame: %.2fms", static_cast<int>(fps), ms);

            FpsFrameCount = 0;
            FpsTimer = 0.0;
        }
        if (FpsFixedTimer >= 1.0)
        {
            double fps = FpsFixedCount / FpsFixedTimer;
            double ms = (FpsFixedTimer / FpsFixedCount) * 1000.0;

            LOG_DEBUG_F("Fixed FPS: %d | Frame: %.2fms", static_cast<int>(fps), ms);

            FpsFixedCount = 0;
            FpsFixedTimer = 0.0;
        }

        // Spiral of death cap
        if (dt > kMaxDt) dt = kMaxDt;

        Accumulator += dt;

        // Prevent unbounded catch-up
        if (Accumulator > kMaxAccumulatedTime) Accumulator = kMaxAccumulatedTime;

        // Fixed update loop with substepping
        if (fixedStepTime > 0.0)
        {
            STRIGID_ZONE_C(STRIGID_COLOR_LOGIC);

            int steps = 0;
            while (Accumulator >= fixedStepTime && steps < kMaxPhysSubSteps)
            {
                // FPS tracking
                FpsFixedCount++;
                FpsFixedTimer += fixedStepTime;
                
                PrePhysics(fixedStepTime);
                // insert Sim physics here
                PostPhysics(fixedStepTime);
                Accumulator -= fixedStepTime;
                ++steps;
                SimulationTime += dt;
                
                // Publish completed frame to RenderThread
                PublishCompletedFrame();
            }
        }

        // Scalar update only if we're pretty sure we have time.
        if (fixedStepTime - dt > Accumulator) [[likely]]
            ScalarUpdate(dt);

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
    STRIGID_ZONE_N("Logic_PublishFrame");

    // Get the frame header we just wrote to
    TemporalFrameHeader* header = TemporalCache->GetFrameHeader(++FrameNumber);

    // Fill frame metadata
    header->FrameNumber = FrameNumber;
    header->ActiveEntityCount = static_cast<uint32_t>(RegistryPtr->GetTotalEntityCount());
    header->TotalAllocatedEntities = static_cast<uint32_t>(RegistryPtr->GetTotalEntityCount());

    // Fill ViewState (basic perspective camera)
    float AspectRatio = static_cast<float>(WindowWidth) / static_cast<float>(WindowHeight);
    float Fov = 60.0f * 3.14159f / 180.0f; // 60 degrees in radians
    float ZNear = 0.1f;
    float ZFar = 1000.0f;

    // Perspective projection matrix (column-major for GLSL)
    float F = 1.0f / std::tan(Fov / 2.0f);
    header->ProjectionMatrix.m[0] = F / AspectRatio;
    header->ProjectionMatrix.m[1] = 0.0f;
    header->ProjectionMatrix.m[2] = 0.0f;
    header->ProjectionMatrix.m[3] = 0.0f;

    header->ProjectionMatrix.m[4] = 0.0f;
    header->ProjectionMatrix.m[5] = F;
    header->ProjectionMatrix.m[6] = 0.0f;
    header->ProjectionMatrix.m[7] = 0.0f;

    header->ProjectionMatrix.m[8] = 0.0f;
    header->ProjectionMatrix.m[9] = 0.0f;
    header->ProjectionMatrix.m[10] = ZFar / (ZFar - ZNear);
    header->ProjectionMatrix.m[11] = -(ZFar * ZNear) / (ZFar - ZNear);

    header->ProjectionMatrix.m[12] = 0.0f;
    header->ProjectionMatrix.m[13] = 0.0f;
    header->ProjectionMatrix.m[14] = 1;
    header->ProjectionMatrix.m[15] = 0.0f;

    // View matrix = identity for now (camera at origin)
    // ViewMatrix is already initialized to identity in Matrix4 constructor

    // Camera position at origin
    header->CameraPosition.x = 0.0f;
    header->CameraPosition.y = 0.0f;
    header->CameraPosition.z = 0.0f;

    // TODO: Fill SceneState (sun direction, color)
    header->SunDirection = Vector3{0.0f, -1.0f, 0.0f};
    header->SunColor = Vector3{1.0f, 1.0f, 1.0f};
    header->AmbientIntensity = 0.2f;

    // Publish frame number atomically - RenderThread can now read this frame
    LastCompletedFrame.store(FrameNumber, std::memory_order_release);
}

void LogicThread::WaitForTiming(uint64_t frameStart, uint64_t perfFrequency)
{
    STRIGID_ZONE_N("Logic_WaitTiming");

    const double targetFrameTimeSec = ConfigPtr->GetTargetFrameTime();
    const uint64_t targetTicks = static_cast<uint64_t>(targetFrameTimeSec * static_cast<double>(perfFrequency));
    const uint64_t frameEnd = frameStart + targetTicks;

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
