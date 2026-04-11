#pragma once
#include "NetThreadBase.h"

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

	void HandleMessage(const ReceivedMessage& msg);
	void TickReplication();

private:
	void OnClientDisconnectedCB(uint8_t ownerID);

	ReplicationSystem* Replicator = nullptr;
	FlowManager* FlowMgr          = nullptr;
	World* ServerWorld            = nullptr;
};
