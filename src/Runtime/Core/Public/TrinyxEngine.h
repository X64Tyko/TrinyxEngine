#pragma once
#include <atomic>
#include <functional>
#include <memory>

#include "EngineConfig.h"
#include "Events.h"
#include "FlowManager.h"
#include "GNSContext.h"
#include "NetThread.h"
#include "VulkanContext.h"
#include "VulkanMemory.h"
#include "../../Rendering/Private/FramePacer.h"

class RenderThread;
// Forward declarations
class Registry;
class LogicThread;
class JoltPhysics;
class NetConnectionManager;
class ReplicationSystem;
class World;

// Compile-time renderer selection: EditorRenderer (ImGui overlay) or GameplayRenderer (no-op overlay).
#if TNX_ENABLE_EDITOR
class EditorRenderer;
using RendererType = EditorRenderer;
#else
class GameplayRenderer;
using RendererType = GameplayRenderer;
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
	World* GetDefaultWorld() const { return DefaultWorld.get(); }

	// Convenience: access the default world's registry.
	Registry* GetRegistry() const;
	const EngineConfig* GetConfig() const { return &Config; }
	bool GetJobsInitialized() const { return bJobsInitialized.load(std::memory_order_relaxed); }

	// Test-only: hard-reset the registry (wipes all entities, handles, caches).
	void ResetRegistry() const;
	void ConfirmLocalRecycles() const;

	/// Spawn entities from any thread via the default world's SpawnSync.
	void Spawn(std::function<void(Registry*)> action);

	// Renderer access (needed by EditorContext)
	RendererType* GetRenderer() const { return Render.get(); }
	SDL_Window* GetWindow() const { return EngineWindow; }

	// Networking
	GNSContext* GetGNSContext() const { return const_cast<GNSContext*>(&GNS); }
	NetThread* GetNetThread() const { return Net.get(); }

	/// Lazy-init GNS + NetThread if not already active.
	/// Used by editor PIE to enable networking from Standalone mode.
	bool EnsureNetworking();

	// Game-level PIE hooks — game code binds these in PostInitialize.
	// EditorContext fires them during StartPIE/StopPIE and Play/Stop.
	Callback<void, World*, NetConnectionManager*> OnPIEStarted;
	Callback<void, NetConnectionManager*> OnPIEStopped;
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
	void PumpEvents(); // Handle OS events
	void WaitForTiming(uint64_t frameStart, uint64_t perfFrequency);

	// FPS tracking
	void CalculateFPS();

	// --- Window ---
	SDL_Window* EngineWindow = nullptr;
	SDL_GPUDevice* GpuDevice;
	FramePacer Pacer;

	// --- Networking ---
	GNSContext GNS;
	std::unique_ptr<NetThread> Net;

	// --- Vulkan (owned here, shared across worlds) ---
	VulkanContext VkCtx;
	VulkanMemory VkMem;

	// --- Config ---
	EngineConfig Config;
#if TNX_ENABLE_EDITOR && defined(TNX_ENABLE_ROLLBACK)
	int EditorTemporalFrameCount = 8; // User's original value, stashed for PIE worlds
#endif

	// --- World (owns Registry, Physics, Logic, Input, SpawnSync) ---
	std::unique_ptr<World> DefaultWorld;

	// --- Renderer (shared, reads from active world) ---
	std::unique_ptr<RendererType> Render;

	// --- Lifecycle ---
	std::atomic<bool> bIsRunning{false};
	std::atomic<bool> bJobsInitialized{false};
	std::unique_ptr<FlowManager> FlowManager;

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