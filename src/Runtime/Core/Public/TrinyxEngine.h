#pragma once
#include <atomic>
#include <functional>
#include <memory>

#include "EngineConfig.h"
#include "Events.h"
#include "FlowManagerBase.h"
#include "StreamingManager.h"
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
#endif

class AudioManager;
class RenderThread;
class Registry;
class LogicThreadBase;
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

/** @addtogroup core
 *  @{
 */

/**
 * @brief The Sentinel — main-thread owner of all engine subsystems.
 *
 * Owns the SDL window, Vulkan context and memory, the render and audio subsystems,
 * the job system, and the FlowManager. Does @em not own per-world data (Registry,
 * JoltPhysics, LogicThread) — those live inside each @c WorldBase managed by the FlowManager.
 *
 * Responsibilities:
 * - SDL event pumping (SDL requires events on the main thread).
 * - Window and GPU device lifetime.
 * - VulkanContext + VulkanMemory ownership shared across all worlds.
 * - Thread lifecycle management (Logic, Render, job workers).
 * - Frame pacing (timing only — the RenderThread is GPU-autonomous).
 * - FlowManager ownership and default-world caching.
 */
class TrinyxEngine
{
public:
	TrinyxEngine();
	~TrinyxEngine();
	TrinyxEngine(const TrinyxEngine&)            = delete;
	TrinyxEngine& operator=(const TrinyxEngine&) = delete;

	/**
	 * @brief Parse CLI arguments into @c Config before calling @c Initialize().
	 *
	 * Recognizes: `--server`, `--client <ip>`, `--port <port>`, `--latency <ms>`.
	 */
	void ParseCommandLine(int argc, char* argv[]);

	/**
	 * @brief Create the window, initialize Vulkan, audio, and the job system.
	 * @param title      Window title string.
	 * @param width      Initial window width in pixels.
	 * @param height     Initial window height in pixels.
	 * @param projectDir Project root path; falls back to @c TNX_PROJECT_DIR env var if @c nullptr.
	 * @return @c true on success; @c false if any subsystem failed to initialize.
	 */
	bool Initialize(const char* title, int width, int height, const char* projectDir = nullptr);

	/**
	 * @brief Start all threads and run the main loop until the window is closed.
	 * @tparam GameClass A @c GameManager subclass; its @c PostStart is called after threads are ready.
	 *
	 * Calls @c game.PostStart after Logic/Render threads and the job pool are fully initialized,
	 * then loads @c Config.DefaultState if set (non-editor builds only). Calls @c Shutdown before returning.
	 */
	template <typename GameClass>
	void Run(GameClass& game);

	/// @brief Tear down threads, GPU resources, and all owned subsystems.
	void Shutdown();

	/// @brief Singleton accessor — one engine instance per process.
	static TrinyxEngine& Get()
	{
		static TrinyxEngine instance;
		return instance;
	}

	// --- World and flow access ---

	/// @brief Returns the active default world, or @c nullptr before the FlowManager creates one.
	WorldBase* GetDefaultWorld() const { return DefaultWorld; }
	/// @brief Returns the FlowManager that owns all worlds and game states.
	FlowManagerBase* GetFlowManager() const { return Flow.get(); }

	/// @brief Convenience accessor: returns the default world's entity registry.
	Registry* GetRegistry() const;

	/// @brief Returns the active config (editor overlay when @c TNX_ENABLE_EDITOR, else game config).
	const EngineConfig* GetConfig() const { return &Config; }
	/// @brief Returns the pure game config without editor overrides, used when creating PIE worlds.
	const EngineConfig* GetGameConfig() const { return &GameConfig; }

	/// @brief Returns @c true once the job system is initialized and workers are draining queues.
	bool GetJobsInitialized() const { return bJobsInitialized.load(std::memory_order_relaxed); }

	/// @brief Signal the main loop to exit cleanly at end of the current frame.
	void RequestExit() { bIsRunning.store(false, std::memory_order_release); }

#if TNX_ENABLE_EDITOR
	/// @brief Queue a window operation for the Sentinel to execute on the main thread.
	/// 1 = minimize, 2 = maximize/restore. Safe to call from the render thread.
	void DeferWindowOp(uint8_t op) { PendingWindowOp.store(op, std::memory_order_relaxed); }
	/// @brief Accumulate a window position delta from the render thread; Sentinel applies via SDL_SetWindowPosition.
	void DeferWindowMove(float dx, float dy)
	{
		PendingWinDx.fetch_add(static_cast<int>(dx), std::memory_order_relaxed);
		PendingWinDy.fetch_add(static_cast<int>(dy), std::memory_order_relaxed);
	}
#endif

	/// @brief Test-only: wipe all entities, handles, and caches in the default world's registry.
	void ResetRegistry() const;
	/// @brief Advance the default world's local handle free-pool after the safety window.
	void ConfirmLocalRecycles() const;

	/**
	 * @brief Spawn entities into the default world from any thread.
	 *
	 * Blocks the calling thread until the Logic thread executes @p lambda.
	 * @note @p lambda must satisfy @c ValidJobLambda: trivially copyable, ≤48 bytes, signature @c (uint32_t).
	 */
	template <TrinyxJobs::ValidJobLambda LAMBDA>
	void Spawn(LAMBDA lambda)
	{
		if (DefaultWorld) DefaultWorld->SpawnAndWait(lambda);
	}

// --- Renderer, window, and audio (compiled out in headless builds) ---
#ifndef TNX_HEADLESS
	/// @brief Returns the active renderer — @c EditorRenderer or @c GameplayRenderer depending on build flags.
	RendererType* GetRenderer() const { return Render.get(); }
	/// @brief Returns the SDL window handle.
	SDL_Window* GetWindow() const { return EngineWindow; }
	/// @brief Returns the audio manager.
	AudioManager* GetAudio() const { return Audio.get(); }
#endif
	/// @brief Returns the streaming manager (level loads, chunk activations, future GPU transfers).
	StreamingManager* GetStreamingManager() const { return Streaming.get(); }

// --- Networking (compiled out when TNX_ENABLE_NETWORK is not defined) ---
#ifdef TNX_ENABLE_NETWORK
	/// @brief Returns the GNS context (GameNetworkingSockets library state).
	GNSContext* GetGNSContext() const { return const_cast<GNSContext*>(&GNS); }
	/// @brief Returns the active net thread (@c PIENetThread, @c AuthorityNet, or @c OwnerNet).
	NetThreadType* GetNetThread() const { return Net.get(); }

	/**
	 * @brief Initialize GNS and the net thread if not already active.
	 * @note Used by the editor PIE to enable networking from Standalone mode.
	 * @return @c true if networking is available after the call.
	 */
	bool EnsureNetworking();

	Callback<void, WorldBase*, NetConnectionManager*> OnPIEStarted; ///< Fired by EditorContext at StartPIE; game code binds in PostInitialize.
	Callback<void, NetConnectionManager*> OnPIEStopped;             ///< Fired by EditorContext at StopPIE.
#endif

	Callback<void, WorldBase*> OnPlayStarted; ///< Fired when Play (Local) is clicked in the editor.
	Callback<void> OnPlayStopped;             ///< Fired when Stop (Local) is clicked in the editor.

	/// @brief When set, @c PumpEvents writes input to this world instead of @c DefaultWorld.
	/// @note EditorContext sets this during PIE/Play to route input to the active world.
	WorldBase* InputTargetWorld = nullptr;

private:
#ifdef TNX_ENABLE_EDITOR
	friend class EditorContext;
#endif

	// --- Sentinel tasks (main thread) ---
	void StartThreadsAndJobs(); ///< @brief Pin Logic/Render threads and initialize the job worker pool.
	void RunMainLoop();         ///< @brief Main loop: pump events, pace frames, coordinate the render thread.
#ifndef TNX_HEADLESS
	void PumpEvents(); ///< @brief Drain the SDL event queue and route input to the active world.
#endif
	void WaitForTiming(uint64_t frameStart, uint64_t perfFrequency); ///< @brief Spin-wait to hit the configured frame rate cap.
	void CalculateFPS();                                             ///< @brief Update the Sentinel-side FPS counter approximately once per second.

#ifndef TNX_HEADLESS
	// --- Window ---
	SDL_Window* EngineWindow = nullptr; ///< SDL window handle; null in headless builds.
	SDL_GPUDevice* GpuDevice = nullptr; ///< SDL GPU device handle.
	FramePacer Pacer;                   ///< Frame-rate pacing state (timing only — render is GPU-autonomous).
#endif

	std::unique_ptr<StreamingManager> Streaming; ///< Async batch manager — level loads, chunk activations, future GPU transfers.

#ifdef TNX_ENABLE_NETWORK
	// --- Networking ---
	GNSContext GNS;                     ///< GNS library context; owns the GNS global singleton state.
	std::unique_ptr<NetThreadType> Net; ///< Active net thread (PIENetThread / AuthorityNet / OwnerNet).
#if !TNX_ENABLE_EDITOR
	std::unique_ptr<ReplicationSystem> Replicator; ///< Owned for non-editor builds; editor creates per-PIE-session instances in EditorContext.
#endif
#endif

#ifndef TNX_HEADLESS
	// --- Vulkan (owned here, shared across all worlds) ---
	VulkanContext VkCtx; ///< Vulkan instance, physical device, and logical device.
	VulkanMemory VkMem;  ///< VMA allocator and GPU buffer management.

	// --- Renderer and audio ---
	std::unique_ptr<RendererType> Render; ///< Compile-time selected: EditorRenderer or GameplayRenderer.
	std::unique_ptr<AudioManager> Audio;  ///< SDL3 voice pool and audio event registry.
#endif

	// --- Config ---
	EngineConfig Config;     ///< Active config — editor overlay applied on top of game config when @c TNX_ENABLE_EDITOR.
	EngineConfig GameConfig; ///< Pure game config without editor overrides; used when creating PIE Authority/Owner worlds.

	// --- World (owned by FlowManager, cached here for fast access) ---
	WorldBase* DefaultWorld = nullptr; ///< Non-owning alias; FlowManager holds the authoritative world lifetime.

	// --- Lifecycle ---
	std::atomic<bool> bIsRunning{ false };       ///< True while the main loop is executing.
	std::atomic<bool> bJobsInitialized{ false }; ///< Set to true once the job worker pool is ready; Logic thread polls this gate.
#if TNX_ENABLE_EDITOR
	std::atomic<uint8_t> PendingWindowOp{ 0 }; ///< Deferred window op from render thread: 1=minimize, 2=maximize/restore.
	std::atomic<int> PendingWinDx{ 0 };        ///< Accumulated render-thread window x-move delta (pixels).
	std::atomic<int> PendingWinDy{ 0 };        ///< Accumulated render-thread window y-move delta (pixels).
#endif
	std::unique_ptr<FlowManagerBase> Flow; ///< FlowManager — owns all worlds, game states, and level lifetimes.

	// --- Frame timing ---
	uint64_t LastFrameCounter = 0;   ///< SDL performance counter snapshot from the previous frame.
	double FpsTimer           = 0.0; ///< Accumulated time since the last FPS calculation.
	double LastFPSCheck       = 0.0; ///< Timestamp of the most recent FPS update.
	int FrameCount            = 0;   ///< Frames counted since the last FPS calculation.
};

template <typename GameClass>
void TrinyxEngine::Run(GameClass& game)
{
	StartThreadsAndJobs(); // Pin Logic/Render, init workers
	game.PostStart(*this); // Spawns via Engine.Spawn() — syncs with Brain

	// Editor builds skip the auto-load — EditorContext drives flow/level startup
	// and would double-load into the editor DefaultWorld.
#if !TNX_ENABLE_EDITOR
	// Populate the asset registry from the cooked manifest so LoadLevelByName can
	// resolve asset names without a full editor asset database scan.
	if (Config.ProjectDir[0] != '\0')
	{
		std::string contentRoot = std::string(Config.ProjectDir) + "/content";
		AssetRegistry::Get().SetContentRoot(contentRoot);
		AssetRegistry::Get().LoadManifest(contentRoot.c_str());
	}
	if (Config.DefaultState[0] != '\0')
	{
		Flow->LoadDefaultState(Config.DefaultState);
	}
#endif

	RunMainLoop();
	Shutdown();
}

/** @} */
