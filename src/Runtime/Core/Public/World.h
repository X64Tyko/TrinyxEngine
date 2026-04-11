#pragma once
#include <memory>
#include <functional>
#include <span>
#include <vector>

#include "EngineConfig.h"
#include "Input.h"
#include "RegistryTypes.h"
#include "SpawnSync.h"

class Registry;
class LogicThread;
class JoltPhysics;
class ConstructRegistry;

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
	World();
	~World();

	World(const World&)            = delete;
	World& operator=(const World&) = delete;

	/// Initialize all owned subsystems. Call before Start().
	/// ConstructRegistry is owned by FlowManager — passed in so Session-lifetime
	/// Constructs survive World destruction.
	bool Initialize(const EngineConfig& config, ConstructRegistry* constructRegistry,
					int windowWidth = 1920, int windowHeight = 1080);

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
	InputBuffer* GetNetInput() { return &NetInput; }

	/// Returns the sim input for a specific player on the server, or the local
	/// SimInput when ownerID == 0 (client world or standalone). This is the
	/// single call site for player-driven input regardless of network role.
	InputBuffer* GetInputForPlayer(uint8_t ownerID)
	{
		if (ownerID == 0) return &SimInput;
		InputBuffer* buf = GetPlayerSimInput(ownerID);
		return buf ? buf : &SimInput;
	}
	InputBuffer* GetVizInputForPlayer(uint8_t ownerID)
	{
		if (ownerID == 0) return &VizInput;
		InputBuffer* buf = GetPlayerVizInput(ownerID);
		return buf ? buf : &VizInput;
	}
	/// Returns nullptr if ownerID is 0 or beyond the currently connected player
	/// count. Use EnsurePlayerInputSlot() before the first inject to allocate.
	InputBuffer* GetPlayerSimInput(uint8_t ownerID)
	{
		if (ownerID == 0 || ownerID > MaxServerPlayers || ownerID > PlayerSimInputs.size()) return nullptr;
		return PlayerSimInputs[ownerID - 1].get();
	}
	InputBuffer* GetPlayerVizInput(uint8_t ownerID)
	{
		if (ownerID == 0 || ownerID > MaxServerPlayers || ownerID > PlayerVizInputs.size()) return nullptr;
		return PlayerVizInputs[ownerID - 1].get();
	}
	void EnsurePlayerInputSlot(uint8_t ownerID)
	{
		if (ownerID == 0 || ownerID > MaxServerPlayers) return;
		while (PlayerSimInputs.size() < ownerID) PlayerSimInputs.push_back(std::make_unique<InputBuffer>());
		while (PlayerVizInputs.size() < ownerID) PlayerVizInputs.push_back(std::make_unique<InputBuffer>());
	}

	/// All buffers Sentinel should fan input into. Iterate instead of adding push sites.
	std::span<InputBuffer* const> GetInputTargets() const { return InputTargets; }
	const EngineConfig& GetConfig() const { return Config; }
	EngineConfig& GetConfigMut() { return Config; }
	ConstructRegistry* GetConstructRegistry() { return Constructs; }

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
	InputBuffer NetInput;
	std::vector<std::unique_ptr<InputBuffer>> PlayerSimInputs; // per-player sim input, grown on EnsurePlayerInputSlot
	std::vector<std::unique_ptr<InputBuffer>> PlayerVizInputs; // per-player viz input, grown on EnsurePlayerInputSlot
	InputBuffer* InputTargets[3] = {&SimInput, &VizInput, &NetInput};
	SpawnSync Spawner;

	ConstructRegistry* Constructs = nullptr; // Non-owning — FlowManager owns the registry
	std::atomic<bool> bJobsInitialized{false};
};