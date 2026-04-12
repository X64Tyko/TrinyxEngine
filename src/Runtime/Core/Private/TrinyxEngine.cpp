#include "TrinyxEngine.h"

#include <iostream>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_vulkan.h>

#include "AssetRegistry.h"
#include "EngineConfig.h"
#include "FlowManager.h"
#include "ReflectionRegistry.h"
#include "Logger.h"
#include "LogicThread.h"
#include "Profiler.h"
#include "Registry.h"
#include "ThreadPinning.h"
#include "TrinyxJobs.h"
#include "World.h"
#if TNX_ENABLE_EDITOR
#include "EditorRenderer.h"
#else
#include "GameplayRenderer.h"
#endif
#include "JoltPhysics.h"
#if defined(TNX_ENABLE_NETWORK) && !TNX_ENABLE_EDITOR
#include "ReplicationSystem.h"
#endif

// Define global component/class counters (declared in Types.h / SchemaReflector.h)
namespace Internal
{
	uint32_t g_GlobalComponentCounter(1);
	uint8_t g_GlobalMixinCounter(128); // user mixin IDs start after engine band (0-127)
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

void TrinyxEngine::ParseCommandLine(int argc, char* argv[])
{
	for (int i = 1; i < argc; ++i)
	{
#ifdef TNX_ENABLE_NETWORK
		if (strcmp(argv[i], "--server") == 0)
		{
			Config.Mode = EngineMode::Server;
		}
		else if (strcmp(argv[i], "--client") == 0 && i + 1 < argc)
		{
			Config.Mode = EngineMode::Client;
			strncpy(Config.NetAddress, argv[++i], sizeof(Config.NetAddress) - 1);
			Config.NetAddress[sizeof(Config.NetAddress) - 1] = '\0';
		}
		else if (strcmp(argv[i], "--listen") == 0)
		{
			Config.Mode = EngineMode::ListenServer;
		}
		else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
		{
			Config.NetPort = static_cast<uint16_t>(atoi(argv[++i]));
		}
#else
		(void)argv[i]; // suppress unused warning when networking is disabled
#endif
	}
}

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

	EngineWindow = SDL_CreateWindow(title, width, height,
									SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (!EngineWindow)
	{
		std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
		return false;
	}

	// ---- Vulkan init -----------------------------------------------------
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

	// ---- Config ----------------------------------------------------------
	GameConfig = EngineConfig::LoadProjectConfig(projectDir);
	snprintf(GameConfig.ProjectDir, sizeof(GameConfig.ProjectDir), "%s",
			 (projectDir && projectDir[0] != '\0') ? projectDir : "");

#if TNX_ENABLE_EDITOR
	// Editor config: EditorDefaults.ini overrides (e.g. lower TemporalFrameCount for edit-mode).
	Config = EngineConfig::LoadEditorConfig(projectDir, GameConfig);
#else
	Config = GameConfig;
#endif
	snprintf(Config.ProjectDir, sizeof(Config.ProjectDir), "%s", GameConfig.ProjectDir);

	// Publish all static-init registered types (states, modes, entity types)
	// to the AssetRegistry so they're resolvable by name at runtime.
	ReflectionRegistry::Get().PublishToAssetRegistry();

	// Set the content root so all AssetRegistry paths resolve to absolute paths.
	if (Config.ProjectDir[0] != '\0')
		AssetRegistry::Get().SetContentRoot(std::string(Config.ProjectDir) + "/content");

	Flow = std::make_unique<FlowManager>();
	Flow->Initialize(this, &Config, width, height);

	// ---- GNS + NetThread -------------------------------------------------
#ifdef TNX_ENABLE_NETWORK
	if (Config.Mode != EngineMode::Standalone)
	{
		if (!GNS.Initialize())
		{
			LOG_ERROR("GNSContext::Initialize failed — falling back to Standalone");
			Config.Mode = EngineMode::Standalone;
		}
		else
		{
			Net = std::make_unique<NetThreadType>();
			Net->Initialize(&GNS, &Config);
#if defined(TNX_NET_MODEL_PIE)
			Net->InitChildren();
			Net->SetServerFlow(Flow.get());
#elif defined(TNX_NET_MODEL_SERVER)
			Net->SetFlowManager(Flow.get());
#endif
		}
	}
#endif

	// ---- World (owned by FlowManager) ------------------------------------
	if (!Flow->CreateWorld())
	{
		std::cerr << "FlowManager::CreateWorld failed" << std::endl;
		return false;
	}
	DefaultWorld = Flow->GetWorld();

#if defined(TNX_ENABLE_NETWORK) && !TNX_ENABLE_EDITOR
	// Create ReplicationSystem for all server-role modes (including Standalone — Tick
	// does nothing with zero connections, but RegisterConstruct works correctly).
	if (Config.Mode != EngineMode::Client)
	{
		Replicator = std::make_unique<ReplicationSystem>();
		Replicator->Initialize(DefaultWorld);
		DefaultWorld->SetReplicationSystem(Replicator.get());
#if defined(TNX_NET_MODEL_PIE) || defined(TNX_NET_MODEL_SERVER)
	if (Net) Net->SetReplicationSystem(Replicator.get());
#endif
	}
#endif

	Pacer.Initialize(GpuDevice);

	// ---- Renderer --------------------------------------------------------
	Render = std::make_unique<RendererType>();

	Render->Initialize(DefaultWorld->GetRegistry(), DefaultWorld->GetLogicThread(),
					   &Config, &VkCtx, &VkMem, EngineWindow, DefaultWorld->GetVizInput());
#if TNX_ENABLE_EDITOR
	DefaultWorld->GetLogicThread()->SetSimPaused(true); // Editor starts paused
	Render->SetEngine(this);
#endif

	LOG_INFO("TrinyxEngine initialization complete");
	return true;
}

Registry* TrinyxEngine::GetRegistry() const
{
	return DefaultWorld ? DefaultWorld->GetRegistry() : nullptr;
}

void TrinyxEngine::ResetRegistry() const
{
	if (DefaultWorld) DefaultWorld->ResetRegistry();
}

void TrinyxEngine::ConfirmLocalRecycles() const
{
	if (DefaultWorld) DefaultWorld->ConfirmLocalRecycles();
}

void TrinyxEngine::Spawn(std::function<void(Registry*)> action)
{
	if (DefaultWorld) DefaultWorld->Spawn(std::move(action));
}

#ifdef TNX_ENABLE_NETWORK
bool TrinyxEngine::EnsureNetworking()
{
	if (Net) return true;

	if (!GNS.IsInitialized())
	{
		if (!GNS.Initialize())
		{
			LOG_ERROR("[Engine] EnsureNetworking: GNS init failed");
			return false;
		}
	}

	Net = std::make_unique<NetThreadType>();
	Net->Initialize(&GNS, &Config);
#if defined(TNX_NET_MODEL_PIE)
	Net->InitChildren();
	Net->SetServerFlow(Flow.get());
#elif defined(TNX_NET_MODEL_SERVER)
	Net->SetFlowManager(Flow.get());
#endif
	return true;
}
#endif

void TrinyxEngine::StartThreadsAndJobs()
{
	Flow->StartWorld();
	Render->Start();

	while (!DefaultWorld->GetLogicThread()->IsRunning() || !Render->IsRunning())
	{
		// Spin while we wait so that we don't initialize workers before our Primary threads
	}

	// Server model uses inline Tick() from the main loop — no extra thread.
	// Client and PIE models spin a dedicated net thread.
#ifdef TNX_ENABLE_NETWORK
#if defined(TNX_NET_MODEL_SERVER)
	// Inline — no Start(); main loop calls Net->Tick()
#else
	if (Net) Net->Start();
#endif
#endif

	bool JobsInitialized = TrinyxJobs::Initialize(&Config);
	bJobsInitialized.store(JobsInitialized, std::memory_order_release);

	// Notify the world's LogicThread that jobs are ready
	DefaultWorld->SetJobsInitialized(JobsInitialized);

	bIsRunning = true;
}

void TrinyxEngine::RunMainLoop()
{
	const uint64_t perfFrequency = SDL_GetPerformanceFrequency();
	LastFrameCounter             = SDL_GetPerformanceCounter();

	while (bIsRunning.load(std::memory_order_acquire))
	{
		TNX_ZONE_N("Main_Frame");

		const uint64_t frameStart = SDL_GetPerformanceCounter();
		const float dt            = static_cast<float>(static_cast<double>(frameStart - LastFrameCounter) / static_cast<double>(perfFrequency));
		LastFrameCounter          = frameStart;

#if defined(TNX_NET_MODEL_SERVER) && defined(TNX_ENABLE_NETWORK)
		// Dedicated server: main thread is the network poller — no SDL events.
		if (Net) Net->Tick();
#else
		PumpEvents();
#endif

		// Tick the flow state machine — drives FlowState::Tick() on the active state
		Flow->Tick(dt);

		if (DefaultWorld->GetLogicThread() && !DefaultWorld->GetLogicThread()->IsRunning())
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

	// Stop threads — FlowManager owns World lifecycle
	Flow->StopWorld();
	if (Render) Render->Stop();
#ifdef TNX_ENABLE_NETWORK
	if (Net) Net->Stop();
#endif
	Flow->JoinWorld();
	if (Render) Render->Join();
#ifdef TNX_ENABLE_NETWORK
	if (Net) Net->Join();
	Net.reset();
	GNS.Shutdown();
#endif

	// Shut down the job system after coordinator threads have exited
	TrinyxJobs::Shutdown();

	// Destroy thread objects BEFORE Vulkan teardown.
	// RenderThread owns GPU resources that call vmaDestroy* in their destructors.
	Render.reset();

	// FlowManager owns the World — destroy it (and all Constructs) here.
	DefaultWorld = nullptr;
	Flow.reset();

	// Tear down Vulkan
	VkMem.Shutdown();
	VkCtx.Shutdown();

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

	World* targetWorld = InputTargetWorld ? InputTargetWorld : DefaultWorld;
	auto inputTargets  = targetWorld->GetInputTargets();
#ifdef TNX_ENABLE_ROLLBACK
	LogicThread* Logic = targetWorld->GetLogicThread();
#endif

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

			case SDL_EVENT_KEY_DOWN:
#if !defined(TNX_ENABLE_EDITOR)
				if (e.key.scancode == SDL_SCANCODE_ESCAPE)
				{
					bIsRunning.store(false, std::memory_order_release);
					break;
				}
#endif
#ifdef TNX_ENABLE_ROLLBACK
				if (e.key.scancode == SDL_SCANCODE_F5 && !e.key.repeat)
				{
					if (Logic) Logic->RequestRollbackTest();
					break;
				}
#endif
#if TNX_ENABLE_EDITOR
				if (!engineOwnsInput) break;
#endif
				if (!e.key.repeat)
				{
					for (auto* buf : inputTargets) buf->PushKey(e.key.scancode, true);
				}
				break;

			case SDL_EVENT_KEY_UP:
#if TNX_ENABLE_EDITOR
				if (!engineOwnsInput) break;
#endif
				for (auto* buf : inputTargets) buf->PushKey(e.key.scancode, false);
				break;

			case SDL_EVENT_MOUSE_MOTION:
#if TNX_ENABLE_EDITOR
				if (!engineOwnsInput) break;
#endif
				for (auto* buf : inputTargets) buf->AddMouseDelta(e.motion.xrel, e.motion.yrel);
				break;

			case SDL_EVENT_MOUSE_BUTTON_DOWN:
#if !TNX_ENABLE_EDITOR
				SDL_SetWindowRelativeMouseMode(EngineWindow, true);
#else
				if (!engineOwnsInput) break;
#endif
				for (auto* buf : inputTargets) buf->PushMouseButton(e.button.button, true);
				break;

			case SDL_EVENT_MOUSE_BUTTON_UP:
#if TNX_ENABLE_EDITOR
				if (!engineOwnsInput) break;
#endif
				for (auto* buf : inputTargets) buf->PushMouseButton(e.button.button, false);
				break;

			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
			case SDL_EVENT_WINDOW_RESIZED: if (Render) Render->NotifyResize();
				break;

			default: break;
		}
	}
}

/*
void TrinyxEngine::ServiceRenderThread() { ... }
void TrinyxEngine::AcquireAndProvideGPUResources() { ... }
void TrinyxEngine::SubmitRenderCommands() { ... }
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

		constexpr double SleepMarginSec = 0.002;
		if (remainingSec > SleepMarginSec) SDL_Delay(static_cast<uint32_t>((remainingSec - SleepMarginSec) * 1000.0));

		while (SDL_GetPerformanceCounter() < frameEnd)
		{
			/* busy wait */
		}
	}
}