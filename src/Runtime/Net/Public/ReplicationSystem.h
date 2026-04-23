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

class WorldBase;
class Registry;
class ConstructRegistry;
class NetConnectionManager;
union EntityHandle;
union GlobalEntityHandle;

// ---------------------------------------------------------------------------
// ReplicationSystem — server-side entity and Construct replication.
//
// Walks the server world's Registry each network tick and:
//   1. Sends EntitySpawn (reliable) for any entity not yet replicated.
//   2. Sends ConstructSpawn (reliable) for any registered Construct not yet sent.
//   3. Sends batched StateCorrection (unreliable) with authoritative transforms.
//
// Designed for PIE loopback — one server world, N client connections.
// Not optimized: full-state send every tick, no delta compression.
//
// Friend of Registry — uses internal APIs: AllocateNetIndex, GlobalEntityRegistry,
// CreateInternal. All entity references are GlobalEntityHandle, not EntityHandle.
// ---------------------------------------------------------------------------
class ReplicationSystem
{
public:
	ReplicationSystem() = default;

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

	/// Register a Construct for replication. Allocates a ConstructNetHandle, wires
	/// ConstructRegistry Records/NetToRecord, and queues a ConstructSpawn payload
	/// (pre-built with EntityNetHandles resolved immediately) to be sent on the next Tick.
	///
	/// The typed template lets us call CollectViewHandles directly — no fn pointer stored
	/// in ConstructRecord, no deferred collection. EntityNetHandles are resolved against
	/// the server Registry at registration time.
	///
	/// Returns a valid ConstructRef immediately — safe to pass to Soul::ClaimBody.
	template <typename T>
	ConstructRef RegisterConstruct(ConstructRegistry* reg, T* ptr, uint8_t ownerID,
								   uint16_t typeHash, int64_t prefabIDRaw)
	{
		ConstructNetManifest manifest{};
		manifest.PrefabIndex = typeHash;
		manifest.NetFlags    = 0;

		const uint32_t spawnFrame = AuthorityWorld && AuthorityWorld->GetLogicThread()
										? AuthorityWorld->GetLogicThread()->GetLastCompletedFrame()
										: 0;

		ConstructRef ref = reg->AllocateNetRef(ptr, ownerID, manifest, typeHash, prefabIDRaw, spawnFrame);

		// Collect view handles from the typed Construct immediately — typed pointer is in scope.
		constexpr size_t MaxViews = 8;
		EntityHandle viewHandles[MaxViews]{};
		uint8_t viewCount = 0;
		ptr->CollectViewHandles(viewHandles, viewCount);

		// Resolve local EntityHandles → EntityNetHandles against the server Registry.
		// If an entity doesn't have a net handle yet, assign one now so the client
		// can look it up after receiving the EntitySpawn for this entity.
		Registry* entityReg = AuthorityWorld ? AuthorityWorld->GetRegistry() : nullptr;
		uint32_t netHandleValues[MaxViews]{};
		if (entityReg)
		{
			for (uint8_t i = 0; i < viewCount; ++i)
			{
				GlobalEntityHandle gH = entityReg->GlobalEntityRegistry.LookupGlobalHandle(viewHandles[i]);
				if (gH.GetIndex() == 0) continue;
				EntityRecord* entRec = entityReg->GlobalEntityRegistry.Records[gH.GetIndex()];
				if (!entRec) continue;
				if (entRec->NetworkID.NetIndex == 0) entRec->NetworkID = AssignNetHandle(entityReg, gH, ownerID);
				netHandleValues[i] = entRec->NetworkID.Value;
			}
		}

		// Pre-build the ConstructSpawnPayload and append to the pending queue.
		const size_t payloadSize = sizeof(ConstructSpawnPayload) + viewCount * sizeof(uint32_t);
		std::vector<uint8_t> buf(payloadSize, 0);
		auto* payload       = reinterpret_cast<ConstructSpawnPayload*>(buf.data());
		payload->Handle     = ref.Handle.Value;
		payload->Manifest   = manifest.Value;
		payload->SpawnFrame = spawnFrame;
		payload->ViewCount  = viewCount;
		uint32_t* trailing  = reinterpret_cast<uint32_t*>(buf.data() + sizeof(ConstructSpawnPayload));
		for (uint8_t i = 0; i < viewCount; ++i) trailing[i] = netHandleValues[i];

		PendingConstructSpawns.push_back(std::move(buf));

		LOG_ENG_INFO_F("[Replication] RegisterConstruct: ownerID=%u typeHash=%u netIndex=%u views=%u",
					   ownerID, typeHash, ref.Handle.NetIndex, viewCount);
		return ref;
	}

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

	/// Ordered list of owner IDs with live channels — used by AuthoritySim to
	/// iterate only connected players rather than the full MaxOwnerIDs range.
	const std::vector<uint8_t>& GetActiveOwnerIDs() const { return ActiveOwnerIDs; }

private:
	void DispatchSpawnJobs(uint32_t frameNumber);
	void DispatchConstructSpawnJobs(uint32_t frameNumber);
	void DispatchCorrectionJobs(uint32_t frameNumber);
	void FlushSendQueues(NetConnectionManager* connMgr);

	/// Assign an EntityNetHandle to a server entity that hasn't been replicated yet.
	/// Allocates a NetIndex, wires NetToRecord, sets the record's NetworkID.
	EntityNetHandle AssignNetHandle(Registry* reg, GlobalEntityHandle gHandle, uint8_t ownerID = 0);

	WorldBase* AuthorityWorld = nullptr;

	// The most recently published frame whose inputs are fully committed (no more rollback possible).
	// Advanced by AuthoritySim::OnFramePublished each fixed tick.
	// Step 7 (networked despawn) reads this to gate phase-1 graduation.
	uint32_t CommittedFrameHorizon = 0;

	// Per-Owner channels — created on connect, destroyed on disconnect.
	// Slot 0 is never populated. ActiveOwnerIDs tracks which slots are live
	// so dispatch loops avoid iterating all MaxOwnerIDs slots each flush.
	std::array<std::unique_ptr<ServerClientChannel>, MaxOwnerIDs> Channels{};
	std::vector<uint8_t> ActiveOwnerIDs;

	// Pre-built ConstructSpawn payloads pending dispatch to all loaded clients.
	// Built at RegisterConstruct time with resolved EntityNetHandles.
	std::vector<std::vector<uint8_t>> PendingConstructSpawns;

	// Dirty entity cache — rebuilt on Sentinel each DispatchFrameJobs(), read-only by correction build jobs.
	struct DirtyEntityInfo
	{
		uint32_t slabIndex;
		uint32_t netHandleValue;
		uint32_t ownerID;
	};

	std::vector<DirtyEntityInfo> DirtyCache;

	// Per-ownerID resim frame snapshot — rebuilt on Sentinel each DispatchFrameJobs(), read-only by correction build jobs.
	struct ResimSnapshot
	{
		const float* posX  = nullptr;
		const float* posY  = nullptr;
		const float* posZ  = nullptr;
		const float* rotQx = nullptr;
		const float* rotQy = nullptr;
		const float* rotQz = nullptr;
		const float* rotQw = nullptr;
		uint32_t delta     = 0;
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
};
