#pragma once

#include <cstdint>

class WorldBase;
class FlowManagerBase;

/// @brief Declares which engine subsystems a @ref FlowState requires.
///
/// @ref FlowManagerBase reads this during transitions to create, preserve, or
/// destroy subsystems. If the incoming state requires a World and the outgoing
/// state already has one, the World survives. If the incoming state does not
/// need a World, the existing World (and all its scoped Constructs) is destroyed.
struct StateRequirements
{
	bool NeedsWorld      = false; ///< Requires Registry, Physics, and Logic thread.
	bool NeedsLevel      = false; ///< Requires a @c .tnxscene loaded into the World.
	bool NeedsNetSession = false; ///< Requires an active network connection.
	bool AllowsSouls     = true;  ///< Souls (player identities) persist through this state.
};

/// @brief Base class for structural application states.
///
/// A @c FlowState is the top-level context that determines what the engine is
/// doing: main menu, loading screen, in-game, post-match, etc.
///
/// States form a stack managed by @ref FlowManagerBase:
/// - @c TransitionTo() — replaces the entire stack with a new state.
/// - @c PushState() — pushes an overlay (e.g. pause menu over gameplay).
/// - @c PopState() — returns to the previous state.
///
/// States declare their subsystem needs via @ref GetRequirements(); the manager
/// creates and destroys Worlds, NetSessions, etc. as states transition in/out.
///
/// All methods run on the Sentinel (main) thread.
class FlowState
{
public:
	virtual ~FlowState() = default;

	/// Called when this state becomes active (pushed or transitioned to).
	/// world may be nullptr if GetRequirements().NeedsWorld is false.
	virtual void OnEnter(FlowManagerBase& flow, WorldBase* world)
	{
		Flow = &flow;
		(void)world;
	}

	/// Called when this state is being replaced or popped.
	virtual void OnExit()
	{
	}

	/// Called each Sentinel frame while this state is the top of the stack.
	virtual void Tick(SimFloat dt) { (void)dt; }

	/// Called on the Sentinel thread when a net flow event arrives from the NetThread.
	/// eventID is a FlowEventID enum value cast to uint8_t.
	virtual void OnNetEvent(uint8_t eventID) { (void)eventID; }

	/// Declare what this state needs from the engine.
	virtual StateRequirements GetRequirements() const { return {}; }

	/// Display name for debugging/logging.
	virtual const char* GetName() const { return "FlowState"; }

protected:
	FlowState()           = default;
	FlowManagerBase* Flow = nullptr; // Set on OnEnter — always the owning FlowManager.
};
