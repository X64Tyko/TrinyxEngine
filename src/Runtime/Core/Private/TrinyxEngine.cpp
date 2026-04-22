#include "TrinyxEngine.h"

#include <filesystem>
#include <iostream>
#include <SDL3/SDL.h>
#ifndef TNX_HEADLESS
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_vulkan.h>
#endif

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
#ifndef TNX_HEADLESS
#include "AudioManager.h"
#include "../../../Runtime/Audio/Private/AudioInternal.h"
#if TNX_ENABLE_EDITOR
#include "EditorRenderer.h"
#else
#include "GameplayRenderer.h"
#endif
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
		if (strcmp(argv[i], "--headless") == 0)
		{
			Config.Headless = true;
		}
		else if (strcmp(argv[i], "--max-frames") == 0 && i + 1 < argc)
		{
			Config.MaxFrames = atoi(argv[++i]);
		}
#ifdef TNX_ENABLE_NETWORK
		else if (strcmp(argv[i], "--server") == 0)
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
			Config.Mode = EngineMode::Host;
		}
		else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
		{
			Config.NetPort = static_cast<uint16_t>(atoi(argv[++i]));
		}
#else
		else { (void)argv[i]; } // suppress unused warning when networking is disabled
#endif
	}
}

bool TrinyxEngine::Initialize(const char* title, int width, int height, const char* projectDir)
{
	TNX_ZONE_N("Engine_Init");

#ifdef TNX_HEADLESS
	Config.Headless = true;
	(void)title; (void)width; (void)height; // unused in headless builds
#endif

	Logger::Get().Init("TrinyxEngine.log", LogLevel::Debug);
	LOG_ENG_INFO("TrinyxEngine initialization started");
	TrinyxThreading::Initialize();
	// Sentinel pin deferred to after config load — EnableThreadPinning may disable it.

	// ---- SDL init --------------------------------------------------------
	if (Config.Headless)
	{
		// Headless: initialize SDL timer/core only — no video, no window, no GPU.
		SDL_Init(0);
	}
	else
	{
		// ---- Windows timer resolution ------------------------------------
		SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "1");

		if (!SDL_WasInit(SDL_INIT_VIDEO))
		{
			if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO))
			{
				std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
				return false;
			}
		}

#ifndef TNX_HEADLESS
		EngineWindow = SDL_CreateWindow(title, width, height,
										SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
		if (!EngineWindow)
		{
			std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
			return false;
		}

		// ---- Vulkan init -------------------------------------------------
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
#endif // !TNX_HEADLESS
	}

	// ---- Config ----------------------------------------------------------
	// Preserve CLI-set flags that must survive the INI load.
	const bool headlessCLI = Config.Headless;
	const int maxFramesCLI = Config.MaxFrames;

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

	// Re-apply CLI flags that must not be overridden by INI files.
	Config.Headless  = headlessCLI;
	Config.MaxFrames = maxFramesCLI;

	// Apply thread pinning preference now that config is resolved.
	TrinyxThreading::SetPinningEnabled(Config.EnableThreadPinning);
	TrinyxThreading::PinCurrentThread(TrinyxThreading::GetIdealCore(CoreAffinity::Input));

	// Apply per-channel log levels from config (Unset → Info for Engine, Debug for Game).
	{
		const LogLevel engineLevel = (Config.EngineLogLevel >= 0)
										 ? static_cast<LogLevel>(Config.EngineLogLevel)
										 : LogLevel::Info;
		const LogLevel gameLevel = (Config.GameLogLevel >= 0)
									   ? static_cast<LogLevel>(Config.GameLogLevel)
									   : LogLevel::Debug;
		Logger::Get().SetMinLevel(LogChannel::Engine, engineLevel);
		Logger::Get().SetMinLevel(LogChannel::Game, gameLevel);
	}

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
			LOG_ENG_ERROR("GNSContext::Initialize failed — falling back to Standalone");
			Config.Mode = EngineMode::Standalone;
		}
		else
		{
			Net = std::make_unique<NetThreadType>();
			Net->Initialize(&GNS, &Config);
#if defined(TNX_NET_MODEL_PIE)
			Net->InitChildren();
			// Server world wired below, after FlowManager::CreateWorld().
#elif defined(TNX_NET_MODEL_SERVER)
			// AuthorityNet resolves FlowManager via AuthorityWorld->GetFlowManager().
			// SetAuthorityWorld() is called below after CreateWorld().
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
#if defined(TNX_NET_MODEL_PIE)
	if (Net) Net->SetAuthorityWorld(DefaultWorld);
#elif defined(TNX_NET_MODEL_SERVER)
	// Wire the per-player input injector into the server world's LogicThread.
	// PIE wires this in EditorContext after SetAuthorityWorld() is called per-session.
	if (Net) Net->SetAuthorityWorld(DefaultWorld);
	if (Net) Net->WirePlayerInputInjector(DefaultWorld);
#endif
	}
#endif

#ifndef TNX_HEADLESS
	Pacer.Initialize(GpuDevice);

	// ---- Renderer --------------------------------------------------------
	Render = std::make_unique<RendererType>();

	Render->Initialize(DefaultWorld->GetRegistry(), DefaultWorld->GetLogicThread(),
					   &Config, &VkCtx, &VkMem, EngineWindow, DefaultWorld->GetVizInput());
#if TNX_ENABLE_EDITOR
	DefaultWorld->GetLogicThread()->SetSimPaused(true); // Editor starts paused
	Render->SetEngine(this);
#endif

	// ---- Audio -----------------------------------------------------------
	Audio = std::make_unique<AudioManager>();
	Audio->Initialize(Config.MaxAudioVoices);
	Audio::SetManager(Audio.get());
#endif // !TNX_HEADLESS

	LOG_ENG_INFO("TrinyxEngine initialization complete");
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

#ifdef TNX_ENABLE_NETWORK
bool TrinyxEngine::EnsureNetworking()
{
	if (Net) return true;

	if (!GNS.IsInitialized())
	{
		if (!GNS.Initialize())
		{
			LOG_ENG_ERROR("[Engine] EnsureNetworking: GNS init failed");
			return false;
		}
	}

	Net = std::make_unique<NetThreadType>();
	Net->Initialize(&GNS, &Config);
#if defined(TNX_NET_MODEL_PIE)
	Net->InitChildren();
	if (DefaultWorld) Net->SetAuthorityWorld(DefaultWorld);
#elif defined(TNX_NET_MODEL_SERVER)
	// AuthorityNet resolves FlowManager via AuthorityWorld->GetFlowManager().
#endif
	return true;
}
#endif

void TrinyxEngine::StartThreadsAndJobs()
{
	Flow->StartWorld();
#ifndef TNX_HEADLESS
	if (Render) Render->Start();
#endif

	while (!DefaultWorld->GetLogicThread()->IsRunning()
#ifndef TNX_HEADLESS
		|| (Render && !Render->IsRunning())
#endif
	)
	{
		// Spin while we wait so that we don't initialize workers before our Primary threads
	}

	// All net models are now driven from the Sentinel main loop — no dedicated net thread.
#ifdef TNX_ENABLE_NETWORK
	// (no Start() call needed)
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

	uint64_t sentinelFrameCount = 0;
#ifndef TNX_HEADLESS
	double audioAccum = 0.0;
#endif
#ifdef TNX_ENABLE_NETWORK
	double netInputAccum      = 0.0; // gates TickInputSend at InputNetHz (128Hz)
	double netTickAccum       = 0.0; // gates Tick (replication) at NetworkUpdateHz (30Hz)
	const double netInputStep = 1.0 / std::max(1, Config.InputNetHz == EngineConfig::Unset ? 128 : Config.InputNetHz);
	const double netTickStep  = 1.0 / std::max(1, Config.NetworkUpdateHz == EngineConfig::Unset ? 30 : Config.NetworkUpdateHz);
#endif

	while (bIsRunning.load(std::memory_order_acquire))
	{
		TNX_ZONE_N("Main_Frame");

		const uint64_t frameStart = SDL_GetPerformanceCounter();
		const float dt            = static_cast<float>(static_cast<double>(frameStart - LastFrameCounter) / static_cast<double>(perfFrequency));
		LastFrameCounter          = frameStart;

#if defined(TNX_NET_MODEL_SERVER) && defined(TNX_ENABLE_NETWORK)
		// Dedicated server has no SDL window — skip event pump.
#elif !defined(TNX_HEADLESS)
		PumpEvents();

		// Sentinel-driven audio update at AudioUpdateHz (accumulator, no busy-wait).
		if (Audio)
		{
			audioAccum             += dt;
			const double audioStep = 1.0 / std::max(1, Config.AudioUpdateHz);
			while (audioAccum >= audioStep)
			{
				Audio->Update(static_cast<float>(audioStep));
				audioAccum -= audioStep;
			}
		}
#endif

		// Tick the flow state machine — drives FlowState::Tick() on the active state
		Flow->Tick(dt);

#ifdef TNX_ENABLE_NETWORK
		if (Net)
		{
			// PumpMessages every sentinel tick: Poll(0) + recv + HandleMessage (may dispatch jobs).
			Net->PumpMessages();

			netInputAccum += static_cast<double>(dt);
			netTickAccum  += static_cast<double>(dt);

			// Input send gated at InputNetHz (128Hz).
			if (netInputAccum >= netInputStep)
			{
				netInputAccum -= netInputStep;
				Net->TickInputSend();
				// Force sendto immediately — packet is on the wire before sleep.
				GNS.Poll();
			}

			// Replication + clock sync gated at NetworkUpdateHz (30Hz).
			if (netTickAccum >= netTickStep)
			{
				netTickAccum -= netTickStep;
				Net->Tick();
			}
		}
#endif

		if (DefaultWorld->GetLogicThread() && !DefaultWorld->GetLogicThread()->IsRunning())
		{
			LOG_ENG_ERROR("[Sentinel] Logic thread stopped unexpectedly — shutting down");
			bIsRunning.store(false, std::memory_order_release);
		}
#ifndef TNX_HEADLESS
		if (Render && !Render->IsRunning())
		{
			LOG_ENG_ERROR("[Sentinel] Render thread stopped unexpectedly — shutting down");
			bIsRunning.store(false, std::memory_order_release);
		}
#endif

		// MaxFrames: clean exit after N frames (used by CI for smoke tests).
		if (Config.MaxFrames > 0 && ++sentinelFrameCount >= static_cast<uint64_t>(Config.MaxFrames))
		{
			LOG_ENG_INFO("[Sentinel] Reached MaxFrames limit — exiting");
			bIsRunning.store(false, std::memory_order_release);
		}

		if (Config.InputPollHz > 0) WaitForTiming(frameStart, perfFrequency);

		TNX_FRAME_MARK();
		CalculateFPS();
	}
}

void TrinyxEngine::Shutdown()
{
	LOG_ENG_INFO("TrinyxEngine shutting down");

	// Stop threads — FlowManager owns World lifecycle
	Flow->StopWorld();
#ifndef TNX_HEADLESS
	if (Render) Render->Stop();
#endif
	Flow->JoinWorld();
#ifndef TNX_HEADLESS
	if (Render) Render->Join();
#endif
#ifdef TNX_ENABLE_NETWORK
	Net.reset();
	GNS.Shutdown();
#endif

	// Shut down the job system after coordinator threads have exited
	TrinyxJobs::Shutdown();

#ifndef TNX_HEADLESS
	// Destroy thread objects BEFORE Vulkan teardown.
	// RenderThread owns GPU resources that call vmaDestroy* in their destructors.
	Audio::SetManager(nullptr);
	Audio->Shutdown();
	Audio.reset();
	Render.reset();
#endif

	// FlowManager owns the World — destroy it (and all Constructs) here.
	DefaultWorld = nullptr;
	Flow.reset();

#ifndef TNX_HEADLESS
	// Tear down Vulkan
	VkMem.Shutdown();
	VkCtx.Shutdown();

	if (EngineWindow)
	{
		SDL_DestroyWindow(EngineWindow);
		EngineWindow = nullptr;
	}
#endif

	SDL_Quit();
	Logger::Get().Shutdown();
}

#ifndef TNX_HEADLESS
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
#endif // !TNX_HEADLESS

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
#if defined(TNX_DETAILED_METRICS)
		LOG_ENG_INFO_F("Main FPS: %d | Frame: %.2fms",
					   static_cast<int>(FrameCount / FpsTimer),
					   (FpsTimer / FrameCount) * 1000.0);
#endif
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