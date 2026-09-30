#pragma once
#include <memory>
#include <functional>
#include <optional>
#include <span>
#include <vector>
#include <cassert>

#include "EngineConfig.h"
#include "Events.h"
#include "Input.h"
#include "JoltPhysics.h"
#include "NetTypes.h"
#include "PhysicsEvents.h"
#include "TnxName.h"
#include "TrinyxJobs.h"
#include "TrinyxMPSCRing.h"

/** @addtogroup core
 *  @{
 */

class Registry;
class LogicThreadBase;
class JoltPhysics;
class ConstructRegistry;
class ReplicationSystem;
class FlowManagerBase;
class AuthorityNet;
class NetConnectionManager;

/// @brief Fixed-capacity multicast delegate for physics contact events.
/// @note Dispatched by the Logic thread after PullActiveTransforms; all listeners run on the Logic thread.
DEFINE_FIXED_MULTICALLBACK(ContactEventFn, 8, const PhysicsContactEvent&)

/// @brief Audio playback command posted from any thread; Sentinel drains before AudioManager::Update.
struct AudioCommand
{
	TnxName Name;         ///< Registered sound asset name.
	float Volume = 1.f;   ///< Playback volume multiplier.
	float Pitch  = 1.f;   ///< Playback pitch multiplier.
	bool Loop    = false; ///< @c true to loop until explicitly stopped.
};

/**
 * @brief Non-template base class for all World specializations.
 *
 * Owns: Registry, JoltPhysics, LogicThread (via LogicThreadBase), input buffers,
 * and the world-scoped job queue handle. Does @em not own Vulkan resources, SDL_Window,
 * MeshManager, or TrinyxJobs — those are shared across worlds by TrinyxEngine.
 *
 * @see World<TNet,TRollback,TFrame> which derives from WorldBase and owns the concrete LogicThread.
 */
class WorldBase
{
public:
	WorldBase();
	virtual ~WorldBase();

	WorldBase(const WorldBase&)            = delete;
	WorldBase& operator=(const WorldBase&) = delete;

	/// @brief Spawn and start the LogicThread (Brain). Jobs must be initialized before calling.
	void Start();

	/// @brief Signal the LogicThread to stop at the end of the current frame.
	void Stop();

	/// @brief Block the caller until the LogicThread exits.
	void Join();

	/// @brief Shut down and destroy all owned subsystems.
	void Shutdown();

	/**
	 * @brief Submit a job to the world queue, blocking the caller until it completes.
	 *
	 * Executes inline on the calling thread if the Logic thread is not yet running or
	 * the job system has not been initialized — avoids deadlock during engine startup.
	 */
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

	/// @brief Alias for SpawnAndWait.
	template <TrinyxJobs::ValidJobLambda LAMBDA>
	void PostAndWait(LAMBDA lambda)
	{
		SpawnAndWait(lambda);
	}

	/// @brief Submit a job with a counter for dependency tracking.
	template <TrinyxJobs::ValidJobLambda LAMBDA>
	void Spawn(LAMBDA lambda, TrinyxJobs::JobCounter* counter)
	{
		TrinyxJobs::Spawn(lambda, counter, WQHandle);
	}

	/// @brief Fire-and-forget job submission — no dependency tracking.
	template <TrinyxJobs::ValidJobLambda LAMBDA>
	void Post(LAMBDA lambda)
	{
		TrinyxJobs::Post(lambda, WQHandle);
	}

	/// @brief Returns the world-scoped job queue handle.
	TrinyxJobs::WorldQueueHandle GetWorldQueue() const { return WQHandle; }

	// --- Accessors ---

	Registry* GetRegistry() const { return RegistryPtr.get(); }
	JoltPhysics* GetPhysics() const { return Physics.get(); }
	LogicThreadBase* GetLogicThread() const { return Logic.get(); }
	InputBuffer* GetSimInput() { return &SimInput; }
	InputBuffer* GetVizInput() { return &VizInput; }

	/**
	 * @brief Engine-internal: returns the correct sim input buffer for @p ownerID.
	 * @note Gameplay code must use Soul::GetSimInput(world) — it applies SoulRole routing.
	 */
	InputBuffer* GetInputForPlayer(uint8_t ownerID)
	{
		if (ownerID == 0) return &SimInput;
		InputBuffer* buf = GetPlayerSimInput(ownerID);
		return buf ? buf : &SimInput;
	}

	/**
	 * @brief Engine-internal: returns the correct viz input buffer for @p ownerID.
	 * @note Gameplay code must use Soul::GetVizInput(world) — it applies SoulRole routing.
	 */
	InputBuffer* GetVizInputForPlayer(uint8_t ownerID)
	{
		if (ownerID == 0) return &VizInput;
		InputBuffer* buf = GetPlayerVizInput(ownerID);
		return buf ? buf : &VizInput;
	}

	/**
	 * @brief Engine-internal: returns the injected net sim buffer for a remote player slot.
	 * @return @c nullptr if the slot is unallocated — call EnsurePlayerInputSlot() first.
	 */
	InputBuffer* GetPlayerSimInput(uint8_t ownerID)
	{
		if (ownerID == 0 || ownerID > PlayerSimInputs.size()) return nullptr;
		return PlayerSimInputs[ownerID - 1].get();
	}

	/// @brief Returns the viz input buffer for a remote player slot, or @c nullptr if unallocated.
	InputBuffer* GetPlayerVizInput(uint8_t ownerID)
	{
		if (ownerID == 0 || ownerID > PlayerVizInputs.size()) return nullptr;
		return PlayerVizInputs[ownerID - 1].get();
	}

	/// @brief Grow player input slot vectors to accommodate @p ownerID (1-based).
	void EnsurePlayerInputSlot(uint8_t ownerID)
	{
		if (ownerID == 0) return;
		while (PlayerSimInputs.size() < ownerID)
			PlayerSimInputs.push_back(std::make_unique<InputBuffer>());
		while (PlayerVizInputs.size() < ownerID)
			PlayerVizInputs.push_back(std::make_unique<InputBuffer>());
	}

	/**
	 * @brief MPSC ring for outbound input accumulation (client-side only).
	 *
	 * Logic thread pushes one NetInputFrame per sim frame at ProcessSimInput time.
	 * Net thread drains via the issued Consumer. The ring rejects pushes until
	 * EnableInputAccum() is called at Playing state.
	 */
	TrinyxMPSCRing<NetInputFrame>& GetInputAccumRing() { return InputAccumRing; }

	/// @brief Returns the net-thread consumer for the input accumulator ring, or @c nullptr if not issued.
	TrinyxMPSCRing<NetInputFrame>::Consumer* GetInputAccumConsumer()
	{
		return InputAccumConsumer.has_value() ? &InputAccumConsumer.value() : nullptr;
	}

	/// @brief Open the input accumulator gate. Call once at PlayerBeginConfirm (Playing state).
	void EnableInputAccum() { bInputAccumEnabled.store(true, std::memory_order_release); }
	bool IsInputAccumEnabled() const { return bInputAccumEnabled.load(std::memory_order_acquire); }

	/// @brief Returns both input targets (SimInput, VizInput) for batch routing.
	std::span<InputBuffer* const> GetInputTargets() const { return InputTargets; }

	const EngineConfig& GetConfig() const { return Config; }
	EngineConfig& GetConfigMut() { return Config; }
	ConstructRegistry* GetConstructRegistry() { return Constructs; }

	ReplicationSystem* GetReplicationSystem() const { return Replicator; }
	void SetReplicationSystem(ReplicationSystem* repl) { Replicator = repl; }

	/**
	 * @brief Base no-op for rollback correction queuing.
	 * @note Overridden by World<..., RollbackSim, ...> when rollback is enabled.
	 */
	virtual void EnqueueCorrections(std::vector<EntityTransformCorrection> /*corrections*/,
		uint32_t /*earliestClientFrame*/)
	{
	}

	/// @brief Base no-op for predicted correction queuing; overridden when rollback is enabled.
	virtual void EnqueuePredictedCorrections(std::vector<EntityTransformCorrection> /*corrections*/)
	{
	}

	/// @brief Base no-op for spawn-triggered rollback; overridden when rollback is enabled.
	virtual void EnqueueSpawnRollback(uint32_t /*clientFrame*/)
	{
	}

	/**
	 * @brief Base no-op for Authority net binding.
	 * @note Overridden by World<AuthoritySim, ...> to call AuthoritySim::Bind without a static_cast.
	 */
	virtual void BindAuthorityNet(AuthorityNet* /*net*/, NetConnectionManager* /*connMgr*/)
	{
	}

	/// @brief Returns the FlowManagerBase that owns this World.
	FlowManagerBase* GetFlowManager() const { return FlowMgr; }
	/// @brief Set by FlowManagerBase::CreateWorld — do not call externally.
	void SetFlowManager(FlowManagerBase* flow) { FlowMgr = flow; }

	/// @brief Set by the engine after the job system is initialized. Logic thread polls this gate.
	void SetJobsInitialized(bool v) { bJobsInitialized.store(v, std::memory_order_release); }
	bool GetJobsInitialized() const { return bJobsInitialized.load(std::memory_order_relaxed); }

	// --- Contact events ---
	/// @brief Physics contact event multicast; listeners invoked on the Logic thread after PullActiveTransforms.
	ContactEventFn OnContactEvent;

	/**
	 * @brief Post an audio playback command from any thread.
	 * @note Thread-safe. Sentinel drains the ring before AudioManager::Update each frame.
	 */
	void TriggerAudio(TnxName name, float volume = 1.f, float pitch = 1.f, bool loop = false)
	{
		AudioCommand cmd;
		cmd.Name   = name;
		cmd.Volume = volume;
		cmd.Pitch  = pitch;
		cmd.Loop   = loop;
		AudioCmdRing.TryPush(cmd);
	}

	/// @brief Returns the Sentinel-side consumer for the audio command ring, or @c nullptr if not issued.
	TrinyxMPSCRing<AudioCommand>::Consumer* GetAudioCmdConsumer()
	{
		return AudioCmdConsumer.has_value() ? &AudioCmdConsumer.value() : nullptr;
	}

	// --- Network ownership ---

	/// @brief Local Owner ID: 0 = Authority/Solo, 1–255 = client slot.
	uint8_t GetLocalOwnerID() const { return LocalOwnerID; }

	/**
	 * @brief Assign the local Owner ID. Called once at handshake.
	 * @note Asserts on double-assignment with a different value.
	 */
	void SetLocalOwnerID(uint8_t id)
	{
		assert((LocalOwnerID == 0 || LocalOwnerID == id) && "LocalOwnerID assigned twice with different values");
		LocalOwnerID = id;
	}

	/**
	 * @brief Frame delta to convert a local logic frame to the equivalent Authority frame.
	 * @note Always 0 on the Authority (local IS server). Set by OwnerNet at handshake.
	 */
	uint32_t GetServerFrameOffset() const { return ServerFrameOffset; }
	void SetServerFrameOffset(uint32_t offset) { ServerFrameOffset = offset; }

	void ResetRegistry() const;        ///< @brief Test-only: wipe all entities, handles, and caches.
	void ConfirmLocalRecycles() const; ///< @brief Advance the local handle free-pool after the safety window.

protected:
	/// @brief Initialize all owned subsystems except LogicThread (created by the derived World<>).
	bool InitBase(const EngineConfig& config, ConstructRegistry* constructRegistry,
		int windowWidth, int windowHeight);

	EngineConfig Config;

	std::unique_ptr<Registry> RegistryPtr;
	std::unique_ptr<JoltPhysics> Physics;
	std::unique_ptr<LogicThreadBase> Logic; ///< Ownership; World<> std::moves the typed instance here.

	InputBuffer SimInput;
	InputBuffer VizInput;
	std::vector<std::unique_ptr<InputBuffer>> PlayerSimInputs; ///< Per-slot sim buffers for remote players (ownerID - 1 indexed).
	std::vector<std::unique_ptr<InputBuffer>> PlayerVizInputs; ///< Per-slot viz buffers for remote players (ownerID - 1 indexed).
	InputBuffer* InputTargets[2]          = { &SimInput, &VizInput };
	TrinyxJobs::WorldQueueHandle WQHandle = TrinyxJobs::InvalidWorldQueue;

	TrinyxMPSCRing<NetInputFrame> InputAccumRing;                              ///< Client outbound input ring.
	std::optional<TrinyxMPSCRing<NetInputFrame>::Consumer> InputAccumConsumer; ///< Net-thread consumer handle.

	TrinyxMPSCRing<AudioCommand> AudioCmdRing;                              ///< Audio command ring (any thread → Sentinel).
	std::optional<TrinyxMPSCRing<AudioCommand>::Consumer> AudioCmdConsumer; ///< Sentinel consumer handle.

	ConstructRegistry* Constructs = nullptr; ///< Non-owning — FlowManager owns the registry.
	ReplicationSystem* Replicator = nullptr; ///< Non-owning — TrinyxEngine or EditorContext owns it.
	FlowManagerBase* FlowMgr      = nullptr; ///< Non-owning — set by FlowManagerBase::CreateWorld.
	uint32_t ServerFrameOffset    = 0;       ///< Local→Authority frame delta; set by OwnerNet at handshake.
	std::atomic<bool> bJobsInitialized{ false };
	std::atomic<bool> bInputAccumEnabled{ false }; ///< Gated to true at PlayerBeginConfirm (client-side only).

private:
	uint8_t LocalOwnerID = 0;    ///< 0 = Authority/Solo, 1–255 = client. Set once via SetLocalOwnerID.
	bool IsLogicRunning() const; ///< @brief Returns true while the Brain thread is active.
};

/** @} */
