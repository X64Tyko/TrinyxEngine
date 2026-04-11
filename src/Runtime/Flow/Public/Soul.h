#pragma once
#include <cstdint>

#include "RegistryTypes.h"

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
// created. Writes to mutable fields must happen from Sentinel or under the
// spawn handshake contract.
// ---------------------------------------------------------------------------
class Soul
{
public:
	explicit Soul(uint8_t ownerID) : OwnerID(ownerID) {}
	virtual ~Soul() = default;

	Soul(const Soul&)            = delete;
	Soul& operator=(const Soul&) = delete;

	uint8_t GetOwnerID() const { return OwnerID; }
	uint32_t GetInputLead() const { return InputLead; }

	bool HasBody() const { return ConfirmedBodyHandle.IsValid(); }
	ConstructRef GetBodyHandle() const { return ConfirmedBodyHandle; }

	/// Called by the GameMode (via WithSpawnManagement or directly) after the
	/// server confirms a spawn. Stores the handle and fires OnBodyConfirmed().
	void ClaimBody(ConstructRef ref)
	{
		ConfirmedBodyHandle = ref;
		OnBodyConfirmed();
	}

	/// Called by FlowManager after creation — game code can override for init.
	virtual void OnJoined()
	{
	}

	/// Called by FlowManager just before destruction.
	virtual void OnLeft()
	{
	}

	/// Called immediately after ClaimBody() — wire input routing, show HUD, etc.
	virtual void OnBodyConfirmed()
	{
	}

	/// Called when the Body is destroyed (death, despawn, respawn cycle start).
	/// ConfirmedBodyHandle is cleared before this fires.
	virtual void OnBodyLost()
	{
	}

	/// Clears ConfirmedBodyHandle and fires OnBodyLost(). Called by GameMode
	/// on Body destruction (e.g. death, level unload).
	void ReleaseBody()
	{
		ConfirmedBodyHandle = {};
		OnBodyLost();
	}

private:
	friend class FlowManager;

	uint8_t OwnerID                  = 0;  // Assigned at connection — stable for the session
	uint32_t InputLead               = 0;  // Frames client leads the server — set after ClockSync
	ConstructRef ConfirmedBodyHandle = {}; // Valid once ClaimBody() is called; cleared on ReleaseBody()
};
