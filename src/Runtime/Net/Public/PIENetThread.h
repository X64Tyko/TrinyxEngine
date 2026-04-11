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

	/// Register a client handler keyed by its GNS connection handle.
	/// Call this immediately after Connect(), before any Tick() pumping,
	/// so the handler is in place to receive the handshake reply.
	void AddClient(HSteamNetConnection clientHandle, World* world, FlowManager* flow);

	/// Promote an existing client entry from handle-based to OwnerID-based routing.
	/// Call this after the pump loop once the server has assigned an OwnerID.
	void UpdateClientOwnerID(HSteamNetConnection clientHandle, uint8_t ownerID, World* world);

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
		HSteamNetConnection Handle = 0; // client-side GNS handle — used for routing before OwnerID is assigned
		uint8_t OwnerID            = 0; // 0 = unassigned (handshake not yet complete)
		std::unique_ptr<ClientNetThread> Handler;
	};
	std::vector<ClientEntry> Clients;
};

#endif // TNX_ENABLE_EDITOR
