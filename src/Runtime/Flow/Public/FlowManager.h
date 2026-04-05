#pragma once

#include <cstdint>
#include <functional>
#include <memory>

class GameState;
class GameMode;
class World;

// ---------------------------------------------------------------------------
// FlowManager — Manages game flow state stack and travel primitives.
//
// The FlowManager is owned by TrinyxEngine and drives the application-level
// state machine. It manages:
//   - A stack of GameStates (menu, loading, in-game, pause overlay, etc.)
//   - World lifetime (create/destroy based on state requirements)
//   - GameMode lifetime (one per World, set by the active state)
//   - Level loading/unloading
//
// Travel is not a single policy — it's a set of orthogonal tools:
//
//   Lever A — Domain lifetime (what survives?):
//     - Keep World, Swap Level (fast travel, seamless, same server)
//     - Reset World (fresh sim, new GameMode)
//     - Keep nothing (full reset)
//
//   Lever B — Construct lifetime (what survives?):
//     - Persistent Constructs survive World resets via reinitialization
//     - World-scoped Constructs are destroyed with the World
//     - Level-scoped Constructs are destroyed on level change
//
//   Lever C — Network continuity:
//     - Keep NetSession (same server)
//     - Swap NetSession (server handoff)
//
// Bootstrap contract:
//   Engine initializes → FlowManager::LoadState("default state name")
//   → from there, user project code owns the entire flow graph.
//
// Vocabulary:
//   State — flow state, drives the app (menu, loading, gameplay)
//   Mode  — rules runtime, drives the match (server authority)
//   Level — content chunk (.tnxscene)
//
// Thread safety: all FlowManager methods run on the Sentinel thread.
// World creation/destruction is synchronized via the spawn handshake.
// ---------------------------------------------------------------------------
class FlowManager
{
public:
	FlowManager() = default;
	~FlowManager();

	FlowManager(const FlowManager&)            = delete;
	FlowManager& operator=(const FlowManager&) = delete;

	// ----- State registration (code-driven, call in PostInitialize) -----

	using StateFactory = std::function<std::unique_ptr<GameState>()>;
	using ModeFactory  = std::function<std::unique_ptr<GameMode>()>;

	/// Register a named state factory. Name is used by LoadState/TransitionTo.
	void RegisterState(const char* name, StateFactory factory);

	/// Register a named mode factory. Name is used by SetGameMode.
	void RegisterMode(const char* name, ModeFactory factory);

	// ----- State stack operations -----

	/// Replace the entire state stack with a single new state.
	/// This is the primary transition: menu → gameplay, gameplay → results, etc.
	/// Enforces requirements: destroys World if the new state doesn't need one,
	/// creates World if it does.
	void TransitionTo(const char* stateName);

	/// Push an overlay state (pause menu, inventory screen).
	/// The underlying state remains alive but stops receiving Tick().
	void PushState(const char* stateName);

	/// Pop the top overlay state, returning control to the state below.
	void PopState();

	/// Load the default state. Called once during engine bootstrap.
	void LoadDefaultState(const char* stateName);

	// ----- World / Level operations -----

	/// Create a fresh World (Registry, Physics, Logic, Input).
	/// Called automatically by TransitionTo when requirements demand it.
	/// Can also be called manually for advanced flows.
	void CreateWorld();

	/// Destroy the current World and everything scoped to it.
	void DestroyWorld();

	/// Load a level (.tnxscene) into the current World.
	void LoadLevel(const char* levelName);

	/// Unload the current level (despawn all level-scoped entities).
	void UnloadLevel();

	// ----- GameMode -----

	/// Set the active GameMode for the current World.
	/// Previous mode is destroyed. Pass nullptr name to clear.
	void SetGameMode(const char* modeName);

	GameMode* GetGameMode() const { return ActiveMode.get(); }

	// ----- Tick (called by Sentinel each frame) -----

	void Tick(float dt);

	// ----- Accessors -----

	GameState* GetActiveState() const;
	World* GetWorld() const { return ActiveWorld; }
	bool HasWorld() const { return ActiveWorld != nullptr; }

private:
	static constexpr uint32_t MaxStateStack       = 8;
	static constexpr uint32_t MaxRegisteredStates = 32;
	static constexpr uint32_t MaxRegisteredModes  = 16;

	// State stack (index 0 = bottom, StateStackCount-1 = top/active)
	std::unique_ptr<GameState> StateStack[MaxStateStack];
	uint32_t StateStackCount = 0;

	// Registered factories
	struct NamedStateFactory
	{
		const char* Name = nullptr;
		StateFactory Factory;
	};

	struct NamedModeFactory
	{
		const char* Name = nullptr;
		ModeFactory Factory;
	};

	NamedStateFactory RegisteredStates[MaxRegisteredStates];
	uint32_t RegisteredStateCount = 0;

	NamedModeFactory RegisteredModes[MaxRegisteredModes];
	uint32_t RegisteredModeCount = 0;

	// Active subsystems
	World* ActiveWorld = nullptr; // Non-owning — engine owns the World
	std::unique_ptr<GameMode> ActiveMode;

	// Internal helpers
	StateFactory FindStateFactory(const char* name) const;
	ModeFactory FindModeFactory(const char* name) const;
	void EnforceRequirements(GameState* newState);
};