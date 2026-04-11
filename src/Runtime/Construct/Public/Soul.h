#pragma once

#include "Construct.h"

// ---------------------------------------------------------------------------
// Soul — Session-lifetime Construct representing a connected player session.
//
// Soul is the engine anchor for a player's persistent identity across World
// resets, level loads, and respawns. It lives from initial connect to final
// disconnect, outlasting any Body the player spawns into.
//
// Ownership
//   Server: created by GameMode::OnPlayerJoined after ClientRepState reaches
//           Loaded. Destroyed on disconnect or session teardown.
//   Client: created locally after SpawnConfirm is received and the net handle
//           is wired to the predicted Body.
//
// Game code subclasses Soul to add loadout, stats, or UI bindings. The engine
// calls the virtual hooks at the right lifecycle points; game code overrides
// only what it needs.
//
//   class PlayerSoul : public Soul
//   {
//       void OnSpawnConfirmed(uint32_t netHandle, float x, float y, float z) override;
//   };
//
// Tick policy: Soul has no tick methods by default — it is event-driven.
// Subclasses can add tick methods and they will be auto-registered via the
// Construct<T> concept mechanism if the outer class uses Construct<Derived>:
//
//   class PlayerSoul : public Soul, public Construct<PlayerSoul> { ... };
//
// Thread safety: Soul is owned by the Sentinel-thread FlowManager. GameMode
// callbacks run on Sentinel. If Construct ticks are added in a subclass, they
// run on Brain and must follow the spawn handshake contract for any ECS writes.
// ---------------------------------------------------------------------------
class Soul
{
public:
	virtual ~Soul() = default;

	// -----------------------------------------------------------------------
	// Session identity — set at handshake, immutable thereafter.
	// -----------------------------------------------------------------------

	uint8_t  OwnerID      = 0; // NetOwnerID assigned by server at HandshakeAccept
	uint32_t InputLead    = 0; // Frames to lead the server — computed after ClockSync
	uint32_t PendingBodyID  = 0; // ConstructID of locally predicted Body (0 = none)
	uint32_t ActiveNetHandle = 0; // Confirmed body net handle (0 = no active body)

	// -----------------------------------------------------------------------
	// Engine lifecycle hooks — called at the right points; override as needed.
	// -----------------------------------------------------------------------

	/// Server has confirmed spawn: NetHandle wired, auth position provided.
	/// Client: reconcile predicted Body position (teleport if delta > threshold).
	virtual void OnSpawnConfirmed(uint32_t netHandle, float posX, float posY, float posZ)
	{
		(void)netHandle; (void)posX; (void)posY; (void)posZ;
	}

	/// Server rejected the spawn request (reason codes TBD per game mode).
	/// Client: destroy predicted Body, clear PendingBodyID.
	virtual void OnSpawnRejected(uint8_t reason) { (void)reason; }

	/// Active Body was destroyed (server authority, World teardown, or disconnect).
	/// Both sides: clear ActiveNetHandle, update UI/spectate state.
	virtual void OnBodyLost() {}

protected:
	Soul() = default;
};
