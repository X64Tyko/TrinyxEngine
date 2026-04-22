#pragma once
#include "NetTypes.h"
#include "WithLobby.h"
#include "WithSpawnManagement.h"
#include "WithTeamAssignment.h"

class WorldBase;
class Soul;
struct PlayerBeginRequestPayload;

// ---------------------------------------------------------------------------
// GameMode — Server-authoritative rules runtime for a World.
//
// GameMode drives match/session rules: spawn policy, win conditions,
// scoring, round transitions, player slot management. It exists only
// while a World is active and is destroyed with it.
//
// One GameMode per World. The active FlowState decides which GameMode
// to create (or none — a menu state has no GameMode).
//
// GameMode is intentionally NOT a Construct itself. User subclasses that
// need ECS ticks should also inherit from Construct<Derived>:
//
//   class ArenaMode : public GameMode, public Construct<ArenaMode> {
//       void OnPlayerJoined(Soul& soul) override;
//       void ScalarUpdate(SimFloat dt);   // check win condition each frame
//   };
//
// This keeps GameMode a simple inheritable base while letting users opt
// into the Construct tick system when they need it. GameModes that are
// pure logic (no per-frame tick) don't pay for Construct overhead.
//
// GameMode does NOT own the World. It is owned by FlowManager and
// destroyed when the World is destroyed or the state transitions away.
//
// Thread safety: OnPlayerJoined/Left are called from the Sentinel thread
// (via FlowManager). Construct ticks run on Brain. Users must respect
// the spawn handshake contract when creating entities from GameMode hooks.
// ---------------------------------------------------------------------------
class GameMode
{
public:
	virtual ~GameMode();

	/// Called after the GameMode is created and the World is ready.
	virtual void Initialize(WorldBase* world) { OwnerWorld = world; }

	/// Called when a Soul requests to join this World.
	/// Override to implement spawn logic (pick spawn point, create Body, etc.).
	virtual void OnPlayerJoined(Soul& soul) { (void)soul; }

	/// Called when a Soul disconnects or leaves.
	virtual void OnPlayerLeft(Soul& soul) { (void)soul; }

	/// Called when the server receives a PlayerBeginRequest from a client.
	/// Pick a spawn point, create a Body, call soul.ClaimBody(result.Body).
	/// Return PlayerBeginResult::Accepted = true to confirm, false to reject.
	/// The engine sends Confirm/Reject based on the returned result — do not
	/// send wire packets from this method.
	virtual PlayerBeginResult OnPlayerBeginRequest(Soul& soul, const PlayerBeginRequestPayload& req);

	/// Called when the World is about to be destroyed (cleanup before teardown).
	virtual void OnWorldTeardown()
	{
	}

	/// Display name for debugging/logging.
	virtual const char* GetModeName() const { return "GameMode"; }

	WorldBase* GetWorld() const { return OwnerWorld; }

protected:
	GameMode() = default;
	WorldBase* OwnerWorld = nullptr;
};