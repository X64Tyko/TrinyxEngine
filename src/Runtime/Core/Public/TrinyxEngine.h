#pragma once
#include <atomic>
#include <memory>

#include "EngineConfig.h"
#include "RenderThread.h"
#include "VulkanContext.h"
#include "VulkanMemory.h"
#include "../../Rendering/Private/FramePacer.h"

class RenderThread;
// Forward declarations
class Registry;
class LogicThread;
class VulkRender;

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
	void Run();
	void Shutdown();

	// Singleton
	static TrinyxEngine& Get()
	{
		static TrinyxEngine instance;
		return instance;
	}

	Registry* GetRegistry() const { return RegistryPtr.get(); }
	bool GetJobsInitialized() const { return bJobsInitialized.load(std::memory_order_relaxed); }

private:
	// Sentinel Tasks (Main Thread)
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

	// --- Thread Modules ---
	std::unique_ptr<LogicThread> Logic;
	std::unique_ptr<VulkRender> Render;

	// --- Lifecycle ---
	std::atomic<bool> bIsRunning{false};
	std::atomic<bool> bJobsInitialized{false};

	// --- FPS tracking ---
	double FpsTimer     = 0.0;
	double LastFPSCheck = 0.0;
	int FrameCount      = 0;
};