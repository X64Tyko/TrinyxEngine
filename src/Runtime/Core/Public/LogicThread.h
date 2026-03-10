#pragma once
#include <thread>
#include <atomic>

#include "Registry.h"

// Forward declarations
class Registry;
struct EngineConfig;
struct InputState;
struct FramePacket;

/**
 * LogicThread: The Brain
 *
 * Runs simulation at FixedUpdateHz with accumulator/substepping
 * Produces FramePackets for Render thread consumption via triple-buffer mailbox
 * Owns the mailbox setup
 */
class LogicThread
{
public:
	LogicThread()  = default;
	~LogicThread() = default;

	void Initialize(Registry* registry, const EngineConfig* config, int windowWidth, int windowHeight);
	void Start();
	void Stop();
	void Join();

	bool IsRunning() const { return bIsRunning.load(std::memory_order_relaxed); }

	// Mailbox access for RenderThread - returns the last completed frame number
	uint32_t GetLastCompletedFrame() const { return LastCompletedFrame.load(std::memory_order_acquire); }

	// Allow RenderThread to peek at accumulator for interpolation alpha calculation.
	// Relaxed load is intentional: a slightly stale alpha produces a barely-visible
	// interpolation error and is far preferable to synchronization overhead.
	double GetAccumulator() const { return Accumulator.load(std::memory_order_relaxed); }
	double GetFixedAlpha() const;

private:
	void ThreadMain(); // Thread entry point

	// Lifecycle Methods
	void ProcessInput();          // Swap input mailbox (TODO: future feature)
	void ScalarUpdate(double dt); // Variable update (runs every frame)
	void PrePhysics(double dt);   // Fixed update at FixedUpdateHz
	void PostPhysics(double dt);  // Fixed update at FixedUpdateHz

	void PublishCompletedFrame(); // Publish last written frame number to mailbox
	void WaitForTiming(uint64_t frameStart, uint64_t perfFrequency);
	void TrackFPS();

	// References (non-owning)
	Registry* RegistryPtr                   = nullptr;
	const EngineConfig* ConfigPtr           = nullptr;
	class ComponentCacheBase* TemporalCache = nullptr;

	// Input (future)
	InputState* CurrentInput = nullptr;

	// Frame mailbox - last completed frame number that RenderThread can read
	std::atomic<uint32_t> LastCompletedFrame{0};

	// Threading
	std::thread Thread;
	std::atomic<bool> bIsRunning{false};

	// Timing — atomic to allow safe relaxed reads from the RenderThread
	std::atomic<double> Accumulator{0.0};
	double SimulationTime = 0.0;
	uint32_t FrameNumber  = 0;
	int WindowWidth       = 1920;
	int WindowHeight      = 1080;

	// FPS tracking
	uint32_t FpsFrameCount = 0;
	double FpsTimer        = 0.0;

	// FPS tracking
	uint32_t FpsFixedCount = 0;
	double FpsFixedTimer   = 0.0;
	double LastFPSCheck    = 0.0;
};

inline void LogicThread::PrePhysics(double dt)
{
	TNX_ZONE_N("Logic_FixedUpdate");

	RegistryPtr->InvokePrePhys(dt);
}

inline void LogicThread::ScalarUpdate(double dt)
{
	TNX_ZONE_N("Logic_Update");

	RegistryPtr->InvokeScalarUpdate(dt);
}

inline void LogicThread::PostPhysics(double dt)
{
	TNX_ZONE_N("Logic_FixedUpdate");

	RegistryPtr->InvokePostPhys(dt);
}
