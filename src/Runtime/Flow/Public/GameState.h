#pragma once

#include <cstdint>

class World;
class FlowManager;

// ---------------------------------------------------------------------------
// StateRequirements — Declares what engine subsystems a state needs.
//
// Each GameState subclass overrides GetRequirements() to declare its needs.
// FlowManager uses these during transitions to create/destroy/preserve
// subsystems. If a transition moves from a state that requires a World to
// one that doesn't, the World is destroyed (and all World-scoped Constructs
// with it). If both states require a World, it survives the transition.
// ---------------------------------------------------------------------------
struct StateRequirements
{
	bool NeedsWorld      = false; // Registry, Physics, Logic thread
	bool NeedsLevel      = false; // A .tnxscene loaded into the World
	bool NeedsNetSession = false; // Active network connection
	bool AllowsSouls     = true;  // Souls persist through this state
};

// ---------------------------------------------------------------------------
// GameState — Base class for flow states.
//
// States drive the application. A State is the top-level context that
// determines what the engine is doing right now: main menu, loading screen,
// in-game, post-match summary, etc.
//
// States form a stack managed by FlowManager:
//   - TransitionTo() replaces the current state
//   - PushState()    pushes an overlay (pause menu over gameplay)
//   - PopState()     returns to the previous state
//
// Lifecycle:
//   1. FlowManager creates the state
//   2. OnEnter() — state sets up its context (UI, world, etc.)
//   3. Tick() — called each frame while active
//   4. OnExit() — state tears down, FlowManager enforces requirements
//
// States do NOT own Worlds or NetSessions directly. They declare what
// they need via GetRequirements(), and FlowManager manages lifetimes.
// This prevents dangling references and enforces clean transitions.
//
// Thread safety: all GameState methods run on the Sentinel (main) thread.
// ---------------------------------------------------------------------------
class GameState
{
public:
	virtual ~GameState() = default;

	/// Called when this state becomes active (pushed or transitioned to).
	/// world may be nullptr if GetRequirements().NeedsWorld is false.
	virtual void OnEnter(FlowManager& flow, World* world)
	{
		Flow = &flow;
		(void)world;
	}

	/// Called when this state is being replaced or popped.
	virtual void OnExit()
	{
	}

	/// Called each Sentinel frame while this state is the top of the stack.
	virtual void Tick(float dt) { (void)dt; }

	/// Called on the Sentinel thread when a net flow event arrives from the NetThread.
	/// eventID is a FlowEventID enum value cast to uint8_t.
	virtual void OnNetEvent(uint8_t eventID) { (void)eventID; }

	/// Declare what this state needs from the engine.
	virtual StateRequirements GetRequirements() const { return {}; }

	/// Display name for debugging/logging.
	virtual const char* GetName() const { return "GameState"; }

protected:
	GameState() = default;
	FlowManager* Flow = nullptr; // Set on OnEnter — always the owning world's FlowManager.
};
