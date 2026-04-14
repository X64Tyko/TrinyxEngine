#pragma once
#include <cstdint>
#include <vector>

#include "Construct.h"
#include "ConstructRecord.h"
#include "ConstructRegistry.h"
#include "EntityRecord.h"
#include "Logger.h"
#include "NetTypes.h"
#include "RegistryTypes.h"

class World;
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

	void Initialize(World* serverWorld);

	/// Run one replication tick. Call from NetThread at NetworkUpdateHz.
	/// Reads the authoritative frame number from the server's LogicThread.
	void Tick(NetConnectionManager* connMgr);

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

		ConstructRef ref = reg->AllocateNetRef(ptr, ownerID, manifest, typeHash, prefabIDRaw);

		// Collect view handles from the typed Construct immediately — typed pointer is in scope.
		constexpr size_t MaxViews = 8;
		EntityHandle viewHandles[MaxViews]{};
		uint8_t viewCount = 0;
		ptr->CollectViewHandles(viewHandles, viewCount);

		// Resolve local EntityHandles → EntityNetHandles against the server Registry.
		// If an entity doesn't have a net handle yet, assign one now so the client
		// can look it up after receiving the EntitySpawn for this entity.
		Registry* entityReg = ServerWorld ? ServerWorld->GetRegistry() : nullptr;
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
		auto* payload      = reinterpret_cast<ConstructSpawnPayload*>(buf.data());
		payload->Handle    = ref.Handle.Value;
		payload->Manifest  = manifest.Value;
		payload->ViewCount = viewCount;
		uint32_t* trailing = reinterpret_cast<uint32_t*>(buf.data() + sizeof(ConstructSpawnPayload));
		for (uint8_t i = 0; i < viewCount; ++i) trailing[i] = netHandleValues[i];

		PendingConstructSpawns.push_back(std::move(buf));

		LOG_ENG_INFO_F("[Replication] RegisterConstruct: ownerID=%u typeHash=%u netIndex=%u views=%u",
					   ownerID, typeHash, ref.Handle.NetIndex, viewCount);
		return ref;
	}

	/// Spawn an entity on the client Registry using a received EntitySpawnPayload.
	/// Uses CreateInternal (GlobalEntityHandle), wires NetToRecord, writes field data.
	static void HandleEntitySpawn(Registry* reg, const struct EntitySpawnPayload& payload);

	/// Apply state corrections to client entities by looking up NetHandle → GlobalEntityHandle.
	static void HandleStateCorrections(Registry* reg, const struct StateCorrectionEntry* entries, uint32_t count);

	/// Create a client-side Construct from a received ConstructSpawn message.
	/// Looks up the client factory from ReflectionRegistry via PrefabIndex (type hash),
	/// resolves EntityNetHandles to local EntityHandles, calls CreateForReplication,
	/// wires ConstructRecord, and calls Soul::ClaimBody via FlowManager.
	/// Returns false if any required entity is not yet in the client registry — caller
	/// should defer and retry next tick.
	static bool HandleConstructSpawn(ConstructRegistry* reg, Registry* entityReg,
									 World* clientWorld, const uint8_t* data, size_t len);

private:
	void SendSpawns(NetConnectionManager* connMgr, uint32_t frameNumber);
	void SendConstructSpawns(NetConnectionManager* connMgr, uint32_t frameNumber);
	void SendStateCorrections(NetConnectionManager* connMgr, uint32_t frameNumber);

	/// Assign an EntityNetHandle to a server entity that hasn't been replicated yet.
	/// Allocates a NetIndex, wires NetToRecord, sets the record's NetworkID.
	EntityNetHandle AssignNetHandle(Registry* reg, GlobalEntityHandle gHandle, uint8_t ownerID = 0);

	World* ServerWorld = nullptr;

	// Track which cache indices have been replicated (spawned on clients).
	std::vector<bool> Replicated;

	// Pre-built ConstructSpawn payloads pending send to all loaded clients.
	// Payloads include resolved EntityNetHandles — built at RegisterConstruct time.
	std::vector<std::vector<uint8_t>> PendingConstructSpawns;
};
