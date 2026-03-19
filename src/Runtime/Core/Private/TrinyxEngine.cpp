#include "TrinyxEngine.h"

#include <iostream>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_vulkan.h>

#include "EngineConfig.h"
#include "Logger.h"
#include "LogicThread.h"
#include "Profiler.h"
#include "Registry.h"
#include "ThreadPinning.h"
#include "TrinyxJobs.h"
#if TNX_ENABLE_EDITOR
#include "EditorRenderer.h"
#else
#include "GameplayRenderer.h"
#endif
#include "JoltPhysics.h"

// Define global component/class counters (declared in Types.h / SchemaReflector.h)
namespace Internal
{
	uint32_t g_GlobalComponentCounter(1);
	std::array<uint8_t, static_cast<size_t>(CacheTier::MAX)> g_TemporalComponentCounter = []()
	{
		std::array<uint8_t, static_cast<size_t>(CacheTier::MAX)> a;
		a.fill(2); // 0 and 1 reserved for pinned types (TemporalFlags, Transform)
		return a;
	}();
	ClassID g_GlobalClassCounter = 1;
}

TrinyxEngine::TrinyxEngine()  = default;
TrinyxEngine::~TrinyxEngine() = default;

bool TrinyxEngine::Initialize(const char* title, int width, int height, const char* projectDir)
{
	TNX_ZONE_N("Engine_Init");

	Logger::Get().Init("TrinyxEngine.log", LogLevel::Debug);
	LOG_INFO("TrinyxEngine initialization started");

	TrinyxThreading::Initialize();
	TrinyxThreading::PinCurrentThread(TrinyxThreading::GetIdealCore(CoreAffinity::Input));

	// ---- SDL init --------------------------------------------------------
	if (!SDL_WasInit(SDL_INIT_VIDEO))
	{
		if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
		{
			std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
			return false;
		}
	}

	// Create GPU Device
	// GpuDevice = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, nullptr);
	// if (!GpuDevice)
	// {
	// 	std::cerr << "GPU Device Failed: " << SDL_GetError() << std::endl;
	// 	SDL_DestroyWindow(EngineWindow);
	// 	return false;
	// }
	// SDL_WINDOW_VULKAN lets SDL register the window for Vulkan surface creation.
	EngineWindow = SDL_CreateWindow(title, width, height,
									SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (!EngineWindow)
	{
		std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
		return false;
	}

	// Claim the Window for the Device
	// if (!SDL_ClaimWindowForGPUDevice(GpuDevice, EngineWindow))
	// {
	// 	std::cerr << "Claim Window Failed: " << SDL_GetError() << std::endl;
	// 	SDL_DestroyGPUDevice(GpuDevice);
	// 	SDL_DestroyWindow(EngineWindow);
	// 	return false;
	// }

	// ---- Vulkan init -----------------------------------------------------
	// Validation layers on in debug, off in release. VulkanContext reads the
	// flag; change to false here to skip the layer even in debug builds.
#if defined(NDEBUG)
	[[maybe_unused]] constexpr bool enableValidation = false;
#else
	[[maybe_unused]] constexpr bool enableValidation = true;
#endif


	if (!VkCtx.Initialize(EngineWindow, enableValidation))
	{
		std::cerr << "VulkanContext::Initialize failed" << std::endl;
		SDL_DestroyWindow(EngineWindow);
		EngineWindow = nullptr;
		return false;
	}

	if (!VkMem.Initialize(VkCtx))
	{
		std::cerr << "VulkanMemory::Initialize failed" << std::endl;
		VkCtx.Shutdown();
		SDL_DestroyWindow(EngineWindow);
		EngineWindow = nullptr;
		return false;
	}
	// using straight Vulkan is... a bit more difficult than SDL lol.

	// ---- Core systems ----------------------------------------------------
	if (projectDir && projectDir[0] != '\0')
	{
		Config = EngineConfig::LoadFromDirectory(projectDir);
		snprintf(Config.ProjectDir, sizeof(Config.ProjectDir), "%s", projectDir);
	}
	else
	{
		Config = EngineConfig::LoadFromFile("TrinyxDefaults.ini");
	}
#if TNX_ENABLE_EDITOR && defined(TNX_ENABLE_ROLLBACK)
	// Editor's edit-mode world uses Volatile-equivalent frame count to save RAM.
	// PIE will create its own Registry with the user's original TemporalFrameCount.
	EditorTemporalFrameCount  = Config.TemporalFrameCount; // Stash for PIE
	Config.TemporalFrameCount = 3;
#endif
	RegistryPtr = std::make_unique<Registry>(&Config);
	Pacer.Initialize(GpuDevice);

	// ---- Physics ---------------------------------------------------------
	Physics = std::make_unique<JoltPhysics>();
	if (!Physics->Initialize(&Config))
	{
		std::cerr << "JoltPhysics::Initialize failed" << std::endl;
		return false;
	}
	RegistryPtr->SetPhysics(Physics.get());

	// ---- Threads ---------------------------------------------------------
	Logic  = std::make_unique<LogicThread>();
	Render = std::make_unique<RendererType>();

	Logic->Initialize(RegistryPtr.get(), &Config, Physics.get(), &SimInput, &VizInput, width, height);
#if TNX_ENABLE_EDITOR
	Logic->SetSimPaused(true); // Editor starts paused — press Play to simulate
#endif

	Render->Initialize(RegistryPtr.get(), Logic.get(), &Config, &VkCtx, &VkMem, EngineWindow, &VizInput);
#if TNX_ENABLE_EDITOR
	Render->SetEngine(this);
#endif

	LOG_INFO("TrinyxEngine initialization complete");
	return true;
}

void TrinyxEngine::StartThreadsAndJobs()
{
	Logic->Start();
	Render->Start();

	while (!Logic->IsRunning() || !Render->IsRunning())
	{
		// Spin while we wait so that we don't initialize workers before our Primary threads
	}

	bool JobsInitialized = TrinyxJobs::Initialize(&Config);
	bJobsInitialized.store(JobsInitialized, std::memory_order_release);

	bIsRunning = true;
}

void TrinyxEngine::RunMainLoop()
{
	const uint64_t perfFrequency = SDL_GetPerformanceFrequency();

	while (bIsRunning.load(std::memory_order_acquire))
	{
		TNX_ZONE_N("Main_Frame");

		const uint64_t frameStart = SDL_GetPerformanceCounter();

		PumpEvents();

		if (Logic && !Logic->IsRunning())
		{
			LOG_ERROR("[Sentinel] Logic thread stopped unexpectedly — shutting down");
			bIsRunning.store(false, std::memory_order_release);
		}
		if (Render && !Render->IsRunning())
		{
			LOG_ERROR("[Sentinel] Render thread stopped unexpectedly — shutting down");
			bIsRunning.store(false, std::memory_order_release);
		}

		if (Config.InputPollHz > 0) WaitForTiming(frameStart, perfFrequency);

		TNX_FRAME_MARK();
		CalculateFPS();
	}
}

void TrinyxEngine::Shutdown()
{
	LOG_INFO("TrinyxEngine shutting down");

	// Stop threads.  RenderThread calls vkDeviceWaitIdle before its loop exits.
	if (Logic) Logic->Stop();
	if (Render) Render->Stop();
	if (Logic) Logic->Join();
	if (Render) Render->Join();

	// Shut down the job system after coordinator threads have exited —
	// no more jobs will be dispatched, so workers can drain and join.
	TrinyxJobs::Shutdown();

	// Destroy thread objects BEFORE Vulkan teardown.
	// RenderThread owns GPU resources (DepthImage, field buffers, pipelines, etc.)
	// that call vmaDestroy* in their destructors.  VulkanMemory (VMA) must still
	// be alive when those destructors run, so we reset the unique_ptrs here
	// rather than letting them fire in TrinyxEngine's destructor after Shutdown().
	Render.reset();
	Logic.reset();
	Physics.reset();

	// Tear down Vulkan explicitly here — before SDL_Quit() and before the
	// TrinyxEngine singleton's atexit destructor fires.  Validation layers
	// use static memory that is freed before atexit(); calling vkDestroy*
	// from atexit() causes a dispatch-handle crash in the validation layer.
	VkMem.Shutdown(); // VMA allocator — must precede device destruction
	VkCtx.Shutdown(); // device → surface → instance (raii objects cleared)

	if (EngineWindow)
	{
		SDL_DestroyWindow(EngineWindow);
		EngineWindow = nullptr;
	}

	SDL_Quit();
	Logger::Get().Shutdown();
}

void TrinyxEngine::PumpEvents()
{
	TNX_ZONE_N("Input_Poll");

#if TNX_ENABLE_EDITOR
	// The render thread owns the decision of whether the engine gets input.
	// Sentinel just reacts to the atomic flag each pump cycle.
	bool engineOwnsInput = Render && !Render->EditorOwnsKeyboard();

	// Sync SDL relative mouse mode with the render thread's decision.
	if (SDL_GetWindowRelativeMouseMode(EngineWindow) != engineOwnsInput) SDL_SetWindowRelativeMouseMode(EngineWindow, engineOwnsInput);
#endif

	SDL_Event e;
	while (SDL_PollEvent(&e))
	{
#if TNX_ENABLE_EDITOR
		// Forward every event to ImGui before engine processing.
		if (Render) Render->PushImGuiEvent(e);
#endif

		switch (e.type)
		{
			case SDL_EVENT_QUIT: bIsRunning.store(false, std::memory_order_release);
				break;

			case SDL_EVENT_KEY_DOWN: if (e.key.scancode == SDL_SCANCODE_ESCAPE)
				{
					bIsRunning.store(false, std::memory_order_release);
					break;
				}
#if TNX_ENABLE_EDITOR
				if (!engineOwnsInput) break;
#endif
				if (!e.key.repeat)
				{
					SimInput.PushKey(e.key.scancode, true);
					VizInput.PushKey(e.key.scancode, true);
				}
				break;

			case SDL_EVENT_KEY_UP:
#if TNX_ENABLE_EDITOR
				if (!engineOwnsInput) break;
#endif
				SimInput.PushKey(e.key.scancode, false);
				VizInput.PushKey(e.key.scancode, false);
				break;

			case SDL_EVENT_MOUSE_MOTION:
#if TNX_ENABLE_EDITOR
				if (!engineOwnsInput) break;
#endif
				SimInput.AddMouseDelta(e.motion.xrel, e.motion.yrel);
				VizInput.AddMouseDelta(e.motion.xrel, e.motion.yrel);
				break;

#if !TNX_ENABLE_EDITOR
			case SDL_EVENT_MOUSE_BUTTON_DOWN: SDL_SetWindowRelativeMouseMode(EngineWindow, true);
				break;
#endif

			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
			case SDL_EVENT_WINDOW_RESIZED: if (Render) Render->NotifyResize();
				break;

			default: break;
		}
	}
}

/*
void TrinyxEngine::ServiceRenderThread()
{
	TNX_ZONE_N("Service_RenderThread");

	if (!Render) return;

	// Check if RenderThread is ready to submit
	if (Render->ReadyToSubmit())
	{
		SubmitRenderCommands();
	}

	// Check if RenderThread needs GPU resources
	if (Render->NeedsGPUResources())
	{
		AcquireAndProvideGPUResources();
	}
}

void TrinyxEngine::AcquireAndProvideGPUResources()
{
	TNX_ZONE_N("Main_AcquireGPU");

	if (!Pacer.BeginFrame()) return;

	// Acquire command buffer and swapchain texture
	SDL_GPUCommandBuffer* cmdBuf = SDL_AcquireGPUCommandBuffer(GpuDevice);
	if (!cmdBuf)
	{
		// No command buffer available, skip this frame
		return;
	}
	// Provide to RenderThread via Render->ProvideGPUResources(cmd, swapchain)
	SDL_GPUTexture* swapchainTex;
	if (!SDL_AcquireGPUSwapchainTexture(cmdBuf, EngineWindow, &swapchainTex, nullptr, nullptr) || !swapchainTex)
	{
		// Failed to acquire texture
		SDL_CancelGPUCommandBuffer(cmdBuf);
		return;
	}

	Render->ProvideGPUResources(cmdBuf, swapchainTex);
}

void TrinyxEngine::SubmitRenderCommands()
{
	TNX_ZONE_N("Main_SubmitGPU");

	// Retrieve command buffer from RenderThread
	SDL_GPUCommandBuffer* cmdBuf = Render->TakeCommandBuffer();
	if (!cmdBuf)
	{
		LOG_ERROR("[Main] Failed to take command buffer from RenderThread");
		return;
	}

	Pacer.EndFrame(cmdBuf);
	Render->NotifyFrameSubmitted();
}
*/

void TrinyxEngine::CalculateFPS()
{
	FrameCount++;
	const double now = SDL_GetPerformanceCounter() /
		static_cast<double>(SDL_GetPerformanceFrequency());
	FpsTimer     += now - LastFPSCheck;
	LastFPSCheck = now;

	if (FpsTimer >= 1.0) [[unlikely]]
	{
		LOG_DEBUG_F("Main FPS: %d | Frame: %.2fms",
					static_cast<int>(FrameCount / FpsTimer),
					(FpsTimer / FrameCount) * 1000.0);
		FrameCount = 0;
		FpsTimer   = 0.0;
	}
}

void TrinyxEngine::WaitForTiming(uint64_t frameStart, uint64_t perfFrequency)
{
	TNX_ZONE_N("Main_WaitTiming");

	const uint64_t targetTicks =
		static_cast<uint64_t>(1.0 / Config.InputPollHz *
			static_cast<double>(perfFrequency));
	const uint64_t frameEnd = frameStart + targetTicks;

	uint64_t now = SDL_GetPerformanceCounter();
	if (frameEnd > now)
	{
		const double remainingSec =
			static_cast<double>(frameEnd - now) / static_cast<double>(perfFrequency);

		constexpr double kSleepMarginSec = 0.002;
		if (remainingSec > kSleepMarginSec) SDL_Delay(static_cast<uint32_t>((remainingSec - kSleepMarginSec) * 1000.0));

		while (SDL_GetPerformanceCounter() < frameEnd)
		{
			/* busy wait */
		}
	}
}