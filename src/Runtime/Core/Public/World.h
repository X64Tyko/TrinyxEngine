#pragma once
#include <memory>
#include <functional>

#include "ConstructRegistry.h"
#include "EngineConfig.h"
#include "Input.h"
#include "SpawnSync.h"

class Registry;
class LogicThread;
class JoltPhysics;

// ---------------------------------------------------------------------------
// World — A self-contained simulation instance.
//
// Owns: Registry, JoltPhysics, LogicThread, InputBuffers, SpawnSync.
// Does NOT own: VulkanContext, VulkanMemory, SDL_Window, MeshManager, TrinyxJobs.
//
// In Standalone mode, the engine creates exactly one World.
// In editor PIE / networked modes, multiple Worlds can coexist:
//   - A server World (headless, no rendering)
//   - N client Worlds (one rendered per viewport)
// ---------------------------------------------------------------------------
class World
{
public:
	World() = default;
	~World();

	World(const World&)            = delete;
	World& operator=(const World&) = delete;

	/// Initialize all owned subsystems. Call before Start().
	/// windowWidth/windowHeight are used for the camera aspect ratio.
	bool Initialize(const EngineConfig& config, int windowWidth = 1920, int windowHeight = 1080);

	/// Start the LogicThread (Brain). Requires jobs to be initialized first.
	void Start();

	/// Signal the LogicThread to stop.
	void Stop();

	/// Join the LogicThread.
	void Join();

	/// Shut down and destroy all owned subsystems.
	void Shutdown();

	// --- Spawn ---

	/// Spawn entities from any thread via the SpawnSync handshake.
	void Spawn(std::function<void(Registry*)> action)
	{
		Spawner.Spawn(std::move(action), RegistryPtr.get());
	}

	SpawnSync& GetSpawner() { return Spawner; }

	// --- Accessors ---

	Registry* GetRegistry() const { return RegistryPtr.get(); }
	JoltPhysics* GetPhysics() const { return Physics.get(); }
	LogicThread* GetLogicThread() const { return Logic.get(); }
	InputBuffer* GetSimInput() { return &SimInput; }
	InputBuffer* GetVizInput() { return &VizInput; }
	const EngineConfig& GetConfig() const { return Config; }
	EngineConfig& GetConfigMut() { return Config; }
	ConstructRegistry* GetConstructRegistry() { return &Constructs; }

	/// Set by engine after jobs are initialized. LogicThread polls this.
	void SetJobsInitialized(bool v) { bJobsInitialized.store(v, std::memory_order_release); }
	bool GetJobsInitialized() const { return bJobsInitialized.load(std::memory_order_relaxed); }

	// --- Network ownership ---
	uint8_t LocalOwnerID = 0; // This world's owner (0 = server, 1-255 = client)

	// Test-only: hard-reset the registry (wipes all entities, handles, caches).
	void ResetRegistry() const;
	void ConfirmLocalRecycles() const;

private:
	EngineConfig Config;

	std::unique_ptr<Registry> RegistryPtr;
	std::unique_ptr<JoltPhysics> Physics;
	std::unique_ptr<LogicThread> Logic;

	InputBuffer SimInput;
	InputBuffer VizInput;
	SpawnSync Spawner;

	ConstructRegistry Constructs;
	std::atomic<bool> bJobsInitialized{false};
};