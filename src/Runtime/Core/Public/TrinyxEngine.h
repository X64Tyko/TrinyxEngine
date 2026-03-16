#pragma once
#include <atomic>
#include <functional>
#include <memory>

#include "EngineConfig.h"
#include "SpawnSync.h"
#include "Input.h"
#include "RenderThread.h"
#include "VulkanContext.h"
#include "VulkanMemory.h"
#include "../../Rendering/Private/FramePacer.h"

class RenderThread;
// Forward declarations
class Registry;
class LogicThread;
class VulkRender;
class JoltPhysics;

/**
 * TrinyxEngine: The Sentinel (Main Thread)
 *
 * Responsibilities:
 * - SDL event pumping (SDL requires this on the main thread)
 * - Window ownership
 * - VulkanContext + VulkanMemory ownership and lifetime
 * - Thread lifecycle management
 * - Frame pacing (timing only — the RenderThread is GPU-autonomous)
 *
 * The Sentinel no longer participates in GPU resource handoff.
 * All command buffer acquisition, submission, and presentation happen
 * inside VulkRender::ThreadMain().
 */
class TrinyxEngine
{
public:
	TrinyxEngine();
	~TrinyxEngine();
	TrinyxEngine(const TrinyxEngine&)            = delete;
	TrinyxEngine& operator=(const TrinyxEngine&) = delete;

	bool Initialize(const char* title, int width, int height, const char* projectDir = nullptr);

	/// Template Run: accepts the GameManager subclass so PostStart can be
	/// called after threads and jobs are fully initialized.
	template <typename GameClass>
	void Run(GameClass& game);

	void Shutdown();

	// Singleton
	static TrinyxEngine& Get()
	{
		static TrinyxEngine instance;
		return instance;
	}

	Registry* GetRegistry() const { return RegistryPtr.get(); }
	const EngineConfig* GetConfig() const { return &Config; }
	bool GetJobsInitialized() const { return bJobsInitialized.load(std::memory_order_relaxed); }

	/// Spawn entities from any thread. Blocks until the work completes at a
	/// safe sync point in the Logic thread's frame. If already on Logic,
	/// executes immediately. See SpawnSync.h for full documentation.
	void Spawn(std::function<void(Registry*)> action)
	{
		Spawner.Spawn(std::move(action), RegistryPtr.get());
	}

	/// Access the SpawnSync directly (used by LogicThread for SyncPoint/SetLogicThreadId).
	SpawnSync& GetSpawner() { return Spawner; }

private:
#ifdef TNX_ENABLE_EDITOR
	friend class EditorContext;
#endif

	// Sentinel Tasks (Main Thread)
	void StartThreadsAndJobs();
	void RunMainLoop();
	void PumpEvents(); // Handle OS events
	//void ServiceRenderThread();           // Check if RenderThread needs GPU resources or wants to submit
	//void AcquireAndProvideGPUResources(); // Acquire cmd + swapchain, provide to RenderThread
	//void SubmitRenderCommands();          // Take CmdBuffer from RenderThread and submit
	void WaitForTiming(uint64_t frameStart, uint64_t perfFrequency);

	// FPS tracking
	void CalculateFPS();

	// --- Window ---
	SDL_Window* EngineWindow = nullptr;
	SDL_GPUDevice* GpuDevice;
	FramePacer Pacer;

	// --- Vulkan (owned here, passed as pointers to RenderThread) ---
	VulkanContext VkCtx;
	VulkanMemory VkMem;

	// --- Core Systems ---
	std::unique_ptr<Registry> RegistryPtr;
	EngineConfig Config;
#if TNX_ENABLE_EDITOR && defined(TNX_ENABLE_ROLLBACK)
	int EditorTemporalFrameCount = 8; // User's original value, stashed for PIE worlds
#endif

	// --- Thread Modules ---
	std::unique_ptr<JoltPhysics> Physics;
	std::unique_ptr<LogicThread> Logic;
	std::unique_ptr<VulkRender> Render;

	// --- Input ---
	InputBuffer Input;

	// --- Spawn sync ---
	SpawnSync Spawner;

	// --- Lifecycle ---
	std::atomic<bool> bIsRunning{false};
	std::atomic<bool> bJobsInitialized{false};

	// --- FPS tracking ---
	double FpsTimer     = 0.0;
	double LastFPSCheck = 0.0;
	int FrameCount      = 0;
};

template <typename GameClass>
void TrinyxEngine::Run(GameClass& game)
{
	StartThreadsAndJobs(); // Pin Logic/Render, init workers
	game.PostStart(*this); // Spawns via Engine.Spawn() — syncs with Brain
	RunMainLoop();
	Shutdown();
}