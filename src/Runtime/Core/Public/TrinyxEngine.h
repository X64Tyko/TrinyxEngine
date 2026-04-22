#pragma once
#include <atomic>
#include <functional>
#include <memory>

#include "EngineConfig.h"
#include "Events.h"
#include "FlowManager.h"
#include "TrinyxJobs.h"
#include "World.h"
#ifdef TNX_ENABLE_NETWORK
#include "GNSContext.h"
#if defined(TNX_NET_MODEL_PIE)
#include "PIENetThread.h"
using NetThreadType = PIENetThread;
#elif defined(TNX_NET_MODEL_SERVER)
#include "AuthorityNet.h"
using NetThreadType = AuthorityNet;
#else
#include "OwnerNet.h"
using NetThreadType = OwnerNet;
#endif
#endif
#ifndef TNX_HEADLESS
#include "VulkanContext.h"
#include "VulkanMemory.h"
#include "../../Rendering/Private/FramePacer.h"
#include "AudioManager.h"
#endif

class RenderThread;
// Forward declarations
class Registry;
class LogicThread;
class JoltPhysics;
#ifdef TNX_ENABLE_NETWORK
class NetConnectionManager;
class ReplicationSystem;
#endif

// Compile-time renderer selection: EditorRenderer (ImGui overlay) or GameplayRenderer (no-op overlay).
// In headless mode there is no renderer — Render stays nullptr.
#ifndef TNX_HEADLESS
#if TNX_ENABLE_EDITOR
class EditorRenderer;
using RendererType = EditorRenderer;
#else
class GameplayRenderer;
using RendererType = GameplayRenderer;
#endif
#endif

/**
 * TrinyxEngine: The Sentinel (Main Thread)
 *
 * Responsibilities:
 * - SDL event pumping (SDL requires this on the main thread)
 * - Window ownership
 * - VulkanContext + VulkanMemory ownership and lifetime
 * - Thread lifecycle management
 * - Frame pacing (timing only — the RenderThread is GPU-autonomous)
 * - World ownership: manages one or more World instances
 *
 * Each World owns its own Registry, JoltPhysics, LogicThread, InputBuffers,
 * and SpawnSync. The engine owns the GPU resources shared across all worlds.
 */
class TrinyxEngine
{
public:
	TrinyxEngine();
	~TrinyxEngine();
	TrinyxEngine(const TrinyxEngine&)            = delete;
	TrinyxEngine& operator=(const TrinyxEngine&) = delete;

	/// Parse CLI args into Config before Initialize().
	/// Supports: --server, --client <ip>, --port <port>, --latency <ms>
	void ParseCommandLine(int argc, char* argv[]);

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

	// --- World access ---
	World* GetDefaultWorld() const { return DefaultWorld; }
	FlowManager* GetFlowManager() const { return Flow.get(); }

	// Convenience: access the default world's registry.
	Registry* GetRegistry() const;
	const EngineConfig* GetConfig() const { return &Config; }
	const EngineConfig* GetGameConfig() const { return &GameConfig; }
	bool GetJobsInitialized() const { return bJobsInitialized.load(std::memory_order_relaxed); }

	// Test-only: hard-reset the registry (wipes all entities, handles, caches).
	void ResetRegistry() const;
	void ConfirmLocalRecycles() const;

	/// Spawn entities from any thread into the default world (blocks until Logic thread executes).
	/// Lambda must satisfy ValidJobLambda: trivially copyable, ≤48 bytes, accepts (uint32_t).
	template <TrinyxJobs::ValidJobLambda LAMBDA>
	void Spawn(LAMBDA lambda) { if (DefaultWorld) DefaultWorld->SpawnAndWait(lambda); }

	// Renderer access (needed by EditorContext)
#ifndef TNX_HEADLESS
	RendererType* GetRenderer() const { return Render.get(); }
	SDL_Window* GetWindow() const { return EngineWindow; }
	AudioManager* GetAudio() const { return Audio.get(); }
#endif

#ifdef TNX_ENABLE_NETWORK
	// Networking
	GNSContext* GetGNSContext() const { return const_cast<GNSContext*>(&GNS); }
	NetThreadType* GetNetThread() const { return Net.get(); }

	/// Lazy-init GNS + NetThread if not already active.
	/// Used by editor PIE to enable networking from Standalone mode.
	bool EnsureNetworking();

	// Game-level PIE hooks — game code binds these in PostInitialize.
	// EditorContext fires them during StartPIE/StopPIE and Play/Stop.
	Callback<void, World*, NetConnectionManager*> OnPIEStarted;
	Callback<void, NetConnectionManager*> OnPIEStopped;
#endif

	Callback<void, World*> OnPlayStarted; // Fired when Play (Local) is clicked
	Callback<void> OnPlayStopped;         // Fired when Stop (Local) is clicked

	// Input routing — when set, PumpEvents writes to this world instead of DefaultWorld.
	// EditorContext sets this during PIE/Play to route input to the active world.
	World* InputTargetWorld = nullptr;

private:
#ifdef TNX_ENABLE_EDITOR
	friend class EditorContext;
#endif

	// Sentinel Tasks (Main Thread)
	void StartThreadsAndJobs();
	void RunMainLoop();
#ifndef TNX_HEADLESS
	void PumpEvents(); // Handle OS events
#endif
	void WaitForTiming(uint64_t frameStart, uint64_t perfFrequency);

	// FPS tracking
	void CalculateFPS();

#ifndef TNX_HEADLESS
	// --- Window ---
	SDL_Window* EngineWindow = nullptr;
	SDL_GPUDevice* GpuDevice = nullptr;
	FramePacer Pacer;
#endif

#ifdef TNX_ENABLE_NETWORK
	// --- Networking ---
	GNSContext GNS;
	std::unique_ptr<NetThreadType> Net;
#if !TNX_ENABLE_EDITOR
	// ReplicationSystem is owned by the engine for non-editor builds.
	// The editor creates its own per-PIE-session instance in EditorContext.
	std::unique_ptr<ReplicationSystem> Replicator;
#endif
#endif

#ifndef TNX_HEADLESS
	// --- Vulkan (owned here, shared across worlds) ---
	VulkanContext VkCtx;
	VulkanMemory VkMem;
#endif

	// --- Renderer (shared, reads from active world) ---
#ifndef TNX_HEADLESS
	std::unique_ptr<RendererType> Render;
	std::unique_ptr<AudioManager> Audio;
#endif

	// --- Config ---
	EngineConfig Config;     // Active config (editor config when TNX_ENABLE_EDITOR, else game config)
	EngineConfig GameConfig; // Pure game config (no editor overrides) — used by PIE for server/client worlds

	// --- World (owned by FlowManager, cached here for fast access) ---
	World* DefaultWorld = nullptr;

	// --- Lifecycle ---
	std::atomic<bool> bIsRunning{false};
	std::atomic<bool> bJobsInitialized{false};
	std::unique_ptr<FlowManager> Flow;

	// --- Frame timing ---
	uint64_t LastFrameCounter = 0; // SDL performance counter from previous frame
	double FpsTimer           = 0.0;
	double LastFPSCheck       = 0.0;
	int FrameCount            = 0;
};

template <typename GameClass>
void TrinyxEngine::Run(GameClass& game)
{
	StartThreadsAndJobs(); // Pin Logic/Render, init workers
	game.PostStart(*this); // Spawns via Engine.Spawn() — syncs with Brain

	// Auto-load the default flow state if configured.
	// World already exists (created in Initialize), so EnforceRequirements is a no-op
	// for NeedsWorld states. The state's OnEnter drives level loading and mode activation.
	// In editor builds the EditorContext drives flow/level loading — skip the auto-load
	// here to prevent a double load into the editor DefaultWorld.
#if !TNX_ENABLE_EDITOR
	if (Config.DefaultState[0] != '\0')
	{
		Flow->LoadDefaultState(Config.DefaultState);
	}
#endif

	RunMainLoop();
	Shutdown();
}