#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "Construct.h"
#include "ConstructRecord.h"
#include "ConstructRegistry.h"
#include "EntityRecord.h"
#include "Logger.h"
#include "NetTypes.h"
#include "Registry.h"
#include "RegistryTypes.h"
#include "ServerClientChannel.h"
#include "TrinyxJobs.h"
#include "TrinyxMPSCRing.h"

class WorldBase;
class Registry;
class ConstructRegistry;
class NetConnectionManager;
union EntityHandle;
union GlobalEntityHandle;

// Server-side entity and Construct replication. Each network tick:
//   1. EntitySpawn (reliable) for entities not yet replicated per channel.
//   2. ConstructSpawn (reliable) for Constructs registered since last tick.
//   3. StateCorrection (unreliable) heartbeat + resim corrections.
//   4. EntityDelta (unreliable) per-component delta for all dirty entities.
//
// Uses Registry internal APIs (AllocateNetIndex, GlobalEntityRegistry).
// All entity references are GlobalEntityHandle, not EntityHandle.
class ReplicationSystem
{
public:
	ReplicationSystem() = default;

	~ReplicationSystem();

	void Initialize(WorldBase* serverWorld);

	/// Check for newly published logic frames and dispatch spawn/correction build jobs.
	/// Call from Sentinel on every loop tick — no-ops if no new frame is available.
	void DispatchFrameJobs();

	/// Drain each channel's send queue to the wire. Call from Sentinel at NetworkUpdateHz.
	void Flush(NetConnectionManager* connMgr);

	/// Pre-register a server entity with a specific OwnerID. Allocates a NetIndex,
	/// wires NetToRecord, and sets the record's NetworkID. SendSpawns will use the
	/// pre-assigned NetHandle instead of assigning a new one.
	/// Call within a Spawn() lambda on the server world.
	void RegisterEntity(Registry* reg, EntityHandle localHandle, uint8_t ownerID);

	/// Register a Construct for replication. Allocates a ConstructNetHandle, resolves its
	/// view EntityNetHandles, pre-builds the ConstructSpawn payload, and queues it for
	/// the next DispatchFrameJobs. Returns a valid ConstructRef — safe to pass to Soul::ClaimBody.
	template <typename T>
	ConstructRef RegisterConstruct(ConstructRegistry* reg, T* ptr, uint8_t ownerID,
		uint16_t typeHash, int64_t prefabIDRaw)
	{
		std::vector<EntityHandle> viewHandles;
		ptr->CollectViewHandles(viewHandles);
		return RegisterConstructCore(reg, ptr, viewHandles, ownerID, typeHash, prefabIDRaw);
	}

	/// Non-template core of RegisterConstruct: everything that doesn't depend on the Construct type.
	ConstructRef RegisterConstructCore(ConstructRegistry* reg, void* ptr, const std::vector<EntityHandle>& viewHandles,
		uint8_t ownerID, uint16_t typeHash, int64_t prefabIDRaw);

	/// Defer a PlayerBeginConfirm until EntitySpawn/ConstructSpawn are in the SendQueue.
	/// Returns false if no active channel for ownerID (caller falls back to direct send).
	bool EnqueuePlayerConfirm(uint8_t ownerID, uint32_t serverFrame,
		const RPCHeader& rpcHdr, const void* params, uint16_t paramSize);

	/// Record that the server resimulated ownerID's input from serverFrame.
	/// Called from AuthoritySim::OnSimInput when an input mismatch fires.
	/// Thread-safe: atomic min-update so multiple dirty marks coalesce to the earliest.
	void AddPendingResim(uint8_t ownerID, uint32_t serverFrame);

	/// Open a channel for ownerID — initializes the input log and spawn tracking.
	/// logDepth should be max(TemporalFrameCount, maxLead + 1).
	void OpenChannel(uint8_t ownerID, uint32_t logDepth, ConnectionInfo* ci, NetConnectionManager* mgr);

	/// Close and reset the channel for ownerID (called on disconnect).
	void CloseChannel(uint8_t ownerID);

	/// Returns the channel if it is active, nullptr otherwise.
	ServerClientChannel* GetChannelIfActive(uint8_t ownerID);

	/// Advance the committed frame horizon to frameNumber (no-op if already past it).
	/// Called by AuthoritySim::OnFramePublished once per fixed tick.
	/// Step 7 (networked despawn) uses this to gate phase-1 graduation.
	void AdvanceCommittedHorizon(uint32_t frameNumber)
	{
		if (frameNumber > CommittedFrameHorizon) CommittedFrameHorizon = frameNumber;
	}

	uint32_t GetCommittedFrameHorizon() const { return CommittedFrameHorizon; }

	// ---------------------------------------------------------------------------
	// Per-frame diagnostics — reset at the start of each DispatchFrameJobs,
	// accumulated from Sentinel and worker threads. Read by the editor debugger panel.
	// ---------------------------------------------------------------------------
	struct NetFrameStats
	{
		std::atomic<uint32_t> StateCorrectionBytes{ 0 }; // heartbeat + resim corrections only
		std::atomic<uint32_t> EntityDeltaBytes{ 0 };
		std::atomic<uint32_t> EntityDeltaEntityCount{ 0 };
		uint32_t DirtyEntityCount   = 0;     // Sentinel-only
		uint32_t ActiveChannelCount = 0;     // Sentinel-only
		bool bHeartbeatFired        = false; // Sentinel-only
		// Stamped by Sentinel via Commit() at the END of DispatchFrameJobs, after all
		// jobs are dispatched. Panel skips sampling if this hasn't changed since last read,
		// preventing duplicate ring-buffer entries when render ticks faster than Sentinel.
		std::atomic<uint32_t> FrameNumber{ 0 };

		void Reset()
		{
			StateCorrectionBytes.store(0, std::memory_order_relaxed);
			EntityDeltaBytes.store(0, std::memory_order_relaxed);
			EntityDeltaEntityCount.store(0, std::memory_order_relaxed);
			DirtyEntityCount   = 0;
			ActiveChannelCount = 0;
			bHeartbeatFired    = false;
		}

		// Call after all dispatch methods complete. Release ordering ensures all
		// accumulated byte counts are visible to the panel when FrameNumber is read.
		void Commit(uint32_t frame)
		{
			FrameNumber.store(frame, std::memory_order_release);
		}
	};

	const NetFrameStats& GetStats() const { return Stats; }

	/// Block until all in-flight build jobs from the last DispatchFrameJobs() have
	/// completed. Must be called before destroying the ReplicationSystem or any world
	/// data those jobs point to (channels, slab headers, Stats).
	void WaitForBuildJobs() { TrinyxJobs::WaitForCounter(&BuildCounter); }

	/// Ordered list of owner IDs with live channels — used by AuthoritySim to
	/// iterate only connected players rather than the full MaxOwnerIDs range.
	const std::vector<uint8_t>& GetActiveOwnerIDs() const { return ActiveOwnerIDs; }

private:
	void DispatchSpawnJobs(uint32_t frameNumber);
	void DispatchActivateJobs(uint32_t frameNumber);
	void DispatchConstructSpawnJobs(uint32_t frameNumber);
	void DispatchConstructDestroyJobs(uint32_t frameNumber);
	void DispatchCorrectionJobs(uint32_t frameNumber);
	void DispatchDeltaCorrectionJobs(uint32_t frameNumber);
	void FlushSendQueues(NetConnectionManager* connMgr);

	/// Assign an EntityNetHandle to a server entity that hasn't been replicated yet.
	/// Allocates a NetIndex, wires NetToRecord, sets the record's NetworkID.
	EntityNetHandle AssignNetHandle(Registry* reg, GlobalEntityHandle gHandle, uint8_t ownerID = 0);

	static void OnConstructDestroyed(void* ctx, ConstructNetHandle handle);

	// Logic thread pushes; Sentinel drains in DispatchFrameJobs.
	struct ConstructDestroyQueue
	{
		struct Node
		{
			uint32_t Value;
			Node* Next = nullptr;
		};

		ConstructDestroyQueue()                                        = default;
		ConstructDestroyQueue(const ConstructDestroyQueue&)            = delete;
		ConstructDestroyQueue& operator=(const ConstructDestroyQueue&) = delete;
		~ConstructDestroyQueue();

		void Push(uint32_t value);

		uint32_t Drain(std::vector<uint32_t>& out);

		std::atomic<Node*> Head{ nullptr };
	};

	ConstructDestroyQueue PendingConstructDestroys;

	WorldBase* AuthorityWorld       = nullptr;
	ConstructRegistry* ConstructReg = nullptr; // Non-owning; set in RegisterConstruct, cleared in destructor

	// Most recent frame with all player inputs committed — gates phase-1 networked despawn.
	uint32_t CommittedFrameHorizon = 0;

	// Per-Owner channels — created on connect, destroyed on disconnect.
	// Slot 0 is never populated. ActiveOwnerIDs tracks which slots are live
	// so dispatch loops avoid iterating all MaxOwnerIDs slots each flush.
	std::array<std::unique_ptr<ServerClientChannel>, MaxOwnerIDs> Channels{};
	std::vector<uint8_t> ActiveOwnerIDs;

	// Pre-built ConstructSpawn payloads pending dispatch to all loaded clients.
	// Built at RegisterConstruct time with resolved EntityNetHandles.
	std::vector<std::vector<uint8_t>> PendingConstructSpawns;

	struct DirtyEntityInfo
	{
		uint32_t slabIndex;
		uint32_t netHandleValue;
		uint32_t ownerID;
	};

	std::vector<DirtyEntityInfo> DirtyCache;

	struct ResimSnapshot
	{
		const SimFloat* posX  = nullptr;
		const SimFloat* posY  = nullptr;
		const SimFloat* posZ  = nullptr;
		const SimFloat* rotQx = nullptr;
		const SimFloat* rotQy = nullptr;
		const SimFloat* rotQz = nullptr;
		const SimFloat* rotQw = nullptr;
		uint32_t delta        = 0;
	};

	ResimSnapshot ResimCache[MaxOwnerIDs]{};

	// Per-ownerID pending resim server frame. Written from the LogicThread injector
	// (input mismatch); drained by correction build jobs.
	// UINT32_MAX = nothing pending. Min-updated atomically to coalesce dirty marks.
	std::atomic<uint32_t> PendingResimFrames[MaxOwnerIDs];

	// Job counter for all build jobs dispatched in one DispatchFrameJobs(). Drain jobs are
	// dispatched immediately after — no wait needed since they check SendQueue::IsEmpty.
	TrinyxJobs::JobCounter BuildCounter;

	// Last frame number for which spawn/correction jobs were dispatched.
	// Sentinel-only — no atomics needed.
	uint32_t LastDispatchedFrame = 0;

	// StateCorrection is sent only when a per-client resim is pending OR on a slow
	// heartbeat to recover from any EntityDelta loss. EntityDelta covers per-frame
	// state sync; the heartbeat is a consistency backstop, not the primary path.
	// Default: 512 frames = 1 seconds at 512Hz. Tune per game.
	uint32_t CorrectionHeartbeatFrames = 512;
	uint32_t LastCorrectionHeartbeat   = 0; // Sentinel-only

	// Per-frame diagnostics collected during each DispatchFrameJobs pass.
	NetFrameStats Stats;
};
