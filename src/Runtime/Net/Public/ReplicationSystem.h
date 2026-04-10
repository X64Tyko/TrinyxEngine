#pragma once
#include <cstdint>
#include <vector>

#include "RegistryTypes.h"

class World;
class Registry;
class NetConnectionManager;
union EntityHandle;
union GlobalEntityHandle;

// ---------------------------------------------------------------------------
// ReplicationSystem — server-side entity replication.
//
// Walks the server world's Registry each network tick and:
//   1. Sends EntitySpawn (reliable) for any entity not yet replicated.
//   2. Sends batched StateCorrection (unreliable) with authoritative transforms.
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

	/// Spawn an entity on the client Registry using a received EntitySpawnPayload.
	/// Uses CreateInternal (GlobalEntityHandle), wires NetToRecord, writes field data.
	static void HandleEntitySpawn(Registry* reg, const struct EntitySpawnPayload& payload);

	/// Apply state corrections to client entities by looking up NetHandle → GlobalEntityHandle.
	static void HandleStateCorrections(Registry* reg, const struct StateCorrectionEntry* entries, uint32_t count);

private:
	void SendSpawns(NetConnectionManager* connMgr, uint32_t frameNumber);
	void SendStateCorrections(NetConnectionManager* connMgr, uint32_t frameNumber);

	/// Assign an EntityNetHandle to a server entity that hasn't been replicated yet.
	/// Allocates a NetIndex, wires NetToRecord, sets the record's NetworkID.
	EntityNetHandle AssignNetHandle(Registry* reg, GlobalEntityHandle gHandle, uint8_t ownerID = 0);

	World* ServerWorld = nullptr;

	// Track which cache indices have been replicated (spawned on clients).
	// Index = cache entity index, value = true if EntitySpawn was sent.
	std::vector<bool> Replicated;
};