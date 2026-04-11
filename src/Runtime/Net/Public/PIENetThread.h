#pragma once
#ifdef TNX_ENABLE_EDITOR

#include "NetThreadBase.h"
#include "ServerNetThread.h"
#include "ClientNetThread.h"

#include <vector>

class FlowManager;
class ReplicationSystem;
class World;

// ---------------------------------------------------------------------------
// PIENetThread
//
// PIE coordinator. Owns a single GNS transport (shared with children) and
// spins one OS thread. On each Tick it routes messages by bServerSide to the
// appropriate child handler. Children are pure message-handlers — they do NOT
// have their own threads and their Initialize() is not called (they share
// this instance's ConnectionMgr).
//
// Usage:
//   pie.Initialize(gns, config);
//   pie.SetServerWorld(serverWorld);
//   pie.SetServerFlow(serverFlow);      // for TravelNotify level-path query
//   pie.SetReplicationSystem(repl);
//   pie.AddClient(ownerID, clientWorld, clientFlow);
//   pie.Start();                        // spins the shared OS thread
// ---------------------------------------------------------------------------
class PIENetThread : public NetThreadBase<PIENetThread>
{
	friend class NetThreadBase<PIENetThread>;

public:
	void SetServerWorld(World* world);
	void SetServerFlow(FlowManager* flow);
	void SetReplicationSystem(ReplicationSystem* repl);
	void AddClient(uint8_t ownerID, World* world, FlowManager* flow);
	void RemoveClient(uint8_t ownerID);
	void ClearClients();

	/// Call after Initialize() to wire the Server child handler to the shared transport.
	/// Must be called before SetServerWorld / SetServerFlow / AddClient.
	void InitChildren();

	ServerNetThread& GetServer() { return Server; }

private:
	void HandleMessage(const ReceivedMessage& msg);
	void TickReplication();

	ServerNetThread Server;

	struct ClientEntry
	{
		uint8_t OwnerID = 0;
		ClientNetThread Handler;
	};
	std::vector<ClientEntry> Clients;
};

#endif // TNX_ENABLE_EDITOR
