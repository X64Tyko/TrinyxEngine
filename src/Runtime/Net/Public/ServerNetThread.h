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
	void SetServerWorld(World* world) { MapConnectionToWorld(0, world); }

	void SetReplicationSystem(ReplicationSystem* repl) { Replicator = repl; }

	/// Used only to query GetActiveLevelLocalPath() when sending TravelNotify.
	void SetFlowManager(FlowManager* flow) { FlowMgr = flow; }

private:
	void HandleMessage(const ReceivedMessage& msg);
	void TickReplication();

	ReplicationSystem* Replicator = nullptr;
	FlowManager* FlowMgr          = nullptr;
};
