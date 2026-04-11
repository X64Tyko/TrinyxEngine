#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Soul — per-player session object, one per connected OwnerID.
//
// Soul bridges the network layer and the gameplay layer. It is the stable
// identity for a player across the full session — it survives Body death
// and respawns.
//
// One Soul per OwnerID, always. Even in splitscreen, two players on one
// machine each get their own Soul and OwnerID.
//
// Lifetime: Session. Created by FlowManager::OnClientLoaded, destroyed by
// FlowManager::OnClientDisconnected. Not a Construct — it is pure data and
// an event dispatch point. Game code subclasses Soul to add stats, loadouts,
// and other session-persistent data.
//
// Thread safety: Soul is created and destroyed on the Sentinel thread.
// Gameplay reads (GetOwnerID, GetInputLead) are safe from any thread once
// created. Writes to mutable fields (PendingSpawnID) must happen from
// Sentinel or under the spawn handshake contract.
// ---------------------------------------------------------------------------
class Soul
{
public:
	explicit Soul(uint8_t ownerID) : OwnerID(ownerID) {}
	virtual ~Soul() = default;

	Soul(const Soul&)            = delete;
	Soul& operator=(const Soul&) = delete;

	uint8_t  GetOwnerID()    const { return OwnerID; }
	uint32_t GetInputLead()  const { return InputLead; }

	bool HasPendingSpawn()   const { return PendingSpawnID != 0; }
	void SetPendingSpawn(uint32_t id) { PendingSpawnID = id; }
	void ClearPendingSpawn()          { PendingSpawnID = 0; }
	uint32_t GetPendingSpawnID() const { return PendingSpawnID; }

	/// Called by FlowManager after creation — game code can override for init.
	virtual void OnJoined()     {}

	/// Called by FlowManager just before destruction.
	virtual void OnLeft()       {}

private:
	friend class FlowManager;

	uint8_t  OwnerID       = 0; // Assigned at connection — stable for the session
	uint32_t InputLead     = 0; // Frames client leads the server — set after ClockSync
	uint32_t PendingSpawnID = 0; // Non-zero while a SpawnRequest is in flight
};
