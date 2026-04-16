#pragma once
#include <memory>
#include <functional>
#include <span>
#include <vector>

#include "EngineConfig.h"
#include "Input.h"
#include "TrinyxJobs.h"

class Registry;
class LogicThread;
class JoltPhysics;
class ConstructRegistry;
class ReplicationSystem;
class FlowManager;

// ---------------------------------------------------------------------------
// World — A self-contained simulation instance.
//
// Owns: Registry, JoltPhysics, LogicThread, InputBuffers, WorldQueue.
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

	template <TrinyxJobs::ValidJobLambda LAMBDA>
	void SpawnAndWait(LAMBDA lambda)
	{
		// Execute inline if the LogicThread isn't draining yet — avoids deadlock during startup
		// (LogicThread spins on JobsInitialized before it will drain the world queue).
		if (!IsLogicRunning() || !bJobsInitialized.load(std::memory_order_acquire))
		{
			lambda(0);
			return;
		}
		TrinyxJobs::SpawnAndWait(lambda, WQHandle);
	}

	template <TrinyxJobs::ValidJobLambda LAMBDA>
	void PostAndWait(LAMBDA lambda) { SpawnAndWait(lambda); }

	template <TrinyxJobs::ValidJobLambda LAMBDA>
	void Spawn(LAMBDA lambda, TrinyxJobs::JobCounter* counter) { TrinyxJobs::Spawn(lambda, counter, WQHandle); }

	template <TrinyxJobs::ValidJobLambda LAMBDA>
	void Post(LAMBDA lambda) { TrinyxJobs::Post(lambda, WQHandle); }

	TrinyxJobs::WorldQueueHandle GetWorldQueue() const { return WQHandle; }

	// --- Accessors ---

	Registry* GetRegistry() const { return RegistryPtr.get(); }
	JoltPhysics* GetPhysics() const { return Physics.get(); }
	LogicThread* GetLogicThread() const { return Logic.get(); }
	InputBuffer* GetSimInput() { return &SimInput; }
	InputBuffer* GetVizInput() { return &VizInput; }
	InputBuffer* GetNetInput() { return &NetInput; }

	/// Engine-internal: returns the correct sim input buffer for a player by ownerID.
	/// Gameplay code must use Soul::GetSimInput(world) — it applies SoulRole routing.
	InputBuffer* GetInputForPlayer(uint8_t ownerID)
	{
		if (ownerID == 0) return &SimInput;
		InputBuffer* buf = GetPlayerSimInput(ownerID);
		return buf ? buf : &SimInput;
	}
	/// Engine-internal: returns the correct viz input buffer for a player by ownerID.
	/// Gameplay code must use Soul::GetVizInput(world) — it applies SoulRole routing.
	InputBuffer* GetVizInputForPlayer(uint8_t ownerID)
	{
		if (ownerID == 0) return &VizInput;
		InputBuffer* buf = GetPlayerVizInput(ownerID);
		return buf ? buf : &VizInput;
	}
	/// Engine-internal: returns the injected net sim buffer for a remote player slot,
	/// or nullptr if the slot is unallocated. Use EnsurePlayerInputSlot() first.
	InputBuffer* GetPlayerSimInput(uint8_t ownerID)
	{
		if (ownerID == 0 || ownerID > PlayerSimInputs.size()) return nullptr;
		return PlayerSimInputs[ownerID - 1].get();
	}
	InputBuffer* GetPlayerVizInput(uint8_t ownerID)
	{
		if (ownerID == 0 || ownerID > PlayerVizInputs.size()) return nullptr;
		return PlayerVizInputs[ownerID - 1].get();
	}
	void EnsurePlayerInputSlot(uint8_t ownerID)
	{
		if (ownerID == 0) return;
		while (PlayerSimInputs.size() < ownerID) PlayerSimInputs.push_back(std::make_unique<InputBuffer>());
		while (PlayerVizInputs.size() < ownerID) PlayerVizInputs.push_back(std::make_unique<InputBuffer>());
	}

	/// All buffers Sentinel should fan input into. Iterate instead of adding push sites.
	std::span<InputBuffer* const> GetInputTargets() const { return InputTargets; }
	const EngineConfig& GetConfig() const { return Config; }
	EngineConfig& GetConfigMut() { return Config; }
	ConstructRegistry* GetConstructRegistry() { return Constructs; }

	ReplicationSystem* GetReplicationSystem() const { return Replicator; }
	void SetReplicationSystem(ReplicationSystem* repl) { Replicator = repl; }

	/// The FlowManager that owns this World. Set automatically in FlowManager::CreateWorld.
	FlowManager* GetFlowManager() const { return FlowMgr; }
	void SetFlowManager(FlowManager* flow) { FlowMgr = flow; }

	/// Set by engine after jobs are initialized. LogicThread polls this.
	void SetJobsInitialized(bool v) { bJobsInitialized.store(v, std::memory_order_release); }
	bool GetJobsInitialized() const { return bJobsInitialized.load(std::memory_order_relaxed); }

	// --- Network ownership ---
	uint8_t LocalOwnerID = 0; // This world's owner (0 = server, 1-255 = client)

	// Test-only: hard-reset the registry (wipes all entities, handles, caches).
	void ResetRegistry() const;
	void ConfirmLocalRecycles() const;

private:
	bool IsLogicRunning() const;

	EngineConfig Config;

	std::unique_ptr<Registry> RegistryPtr;
	std::unique_ptr<JoltPhysics> Physics;
	std::unique_ptr<LogicThread> Logic;

	InputBuffer SimInput;
	InputBuffer VizInput;
	InputBuffer NetInput;
	std::vector<std::unique_ptr<InputBuffer>> PlayerSimInputs; // per-player sim input, grown on EnsurePlayerInputSlot
	std::vector<std::unique_ptr<InputBuffer>> PlayerVizInputs; // per-player viz input, grown on EnsurePlayerInputSlot
	InputBuffer* InputTargets[3]          = {&SimInput, &VizInput, &NetInput};
	TrinyxJobs::WorldQueueHandle WQHandle = TrinyxJobs::InvalidWorldQueue;

	ConstructRegistry* Constructs = nullptr; // Non-owning — FlowManager owns the registry
	ReplicationSystem* Replicator = nullptr; // Non-owning — TrinyxEngine or EditorContext owns it
	FlowManager*       FlowMgr    = nullptr; // Non-owning — set by FlowManager::CreateWorld
	std::atomic<bool> bJobsInitialized{false};
};