#pragma once
#include "NetThreadBase.h"
#include "PlayerInputLog.h"
#include "RegistryTypes.h"
#include <array>
#include <memory>

class FlowManager;
class ReplicationSystem;
class World;

// ---------------------------------------------------------------------------
// ServerNetThread
//
// Handles all server-side message routing:
//   ConnectionHandshake (server accept)  InputFrame  Ping/Pong
//   ClockSync (server respond + TravelNotify)  LevelReady
//
// Owns the server world pointer (non-owning ref) and replication system
// (also non-owning — caller manages lifetime).
//
// FlowManager is used only to query the active level path when sending
// TravelNotify. It is the server FlowManager and never routes client events.
// ---------------------------------------------------------------------------
class ServerNetThread : public NetThreadBase<ServerNetThread>
{
	friend class NetThreadBase<ServerNetThread>;

public:
	/// Non-owning. Set before Start() / first Tick().
	void SetServerWorld(World* world) { ServerWorld = world; }

	void SetReplicationSystem(ReplicationSystem* repl) { Replicator = repl; }

	/// Used only to query GetActiveLevelLocalPath() when sending TravelNotify,
	/// and to call OnClientLoaded/OnClientDisconnected for Soul lifecycle.
	void SetFlowManager(FlowManager* flow);

	/// Called after ConnectionMgr is valid (post Initialize/InitAsHandler).
	/// Registers Soul lifecycle callbacks on the connection manager.
	void BindSoulCallbacks();

	/// LogicThread calls this each sim tick to resolve player input.
	/// Returns nullptr if no log exists for this ownerID (not connected).
	PlayerInputLog* GetInputLog(uint8_t ownerID)
	{
		return (ownerID < MaxOwnerIDs) ? InputLogs[ownerID].get() : nullptr;
	}

	/// Wire the per-player input injector into the world's LogicThread.
	/// Call after SetServerWorld() — the injector runs each sim tick inside ProcessSimInput,
	/// pulling ConsumeFrame() results from each connected player's log and injecting into
	/// their InputBuffer so gameplay code reads correct per-player input.
	void WirePlayerInputInjector(World* world);

	void HandleMessage(const ReceivedMessage& msg);
	void TickReplication();

private:
	void OnClientDisconnectedCB(uint8_t ownerID);

	/// Creates a PlayerInputLog for ownerID, sized to match the temporal slab.
	/// Called from the ConnectionHandshake handler when an ownerID is assigned.
	void CreateInputLog(uint8_t ownerID);

	ReplicationSystem* Replicator = nullptr;
	FlowManager* FlowMgr          = nullptr;
	World* ServerWorld            = nullptr;

	// One log per ownerID slot — only allocated for connected players.
	// Slot 0 (server) is never populated. Depth == TemporalFrameCount.
	std::array<std::unique_ptr<PlayerInputLog>, MaxOwnerIDs> InputLogs{};
};
