#pragma once
#ifdef TNX_ENABLE_EDITOR

#include "NetThreadBase.h"
#include "ServerNetThread.h"
#include "ClientNetThread.h"

#include <vector>

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
// FlowManager is always resolved from world->GetFlowManager() — no separate
// flow pointers needed. Each World knows its owning FlowManager.
//
// Usage:
//   pie.Initialize(gns, config);
//   pie.SetServerWorld(serverWorld);
//   pie.SetReplicationSystem(repl);
//   pie.AddClient(ownerID, clientWorld);
//   pie.Start();                        // spins the shared OS thread
// ---------------------------------------------------------------------------
class PIENetThread : public NetThreadBase<PIENetThread>
{
	friend class NetThreadBase<PIENetThread>;

public:
	void SetServerWorld(World* world);
	void SetReplicationSystem(ReplicationSystem* repl);

	/// Register a client handler keyed by its GNS connection handle.
	/// Call this immediately after Connect(), before any Tick() pumping,
	/// so the handler is in place to receive the handshake reply.
	void AddClient(HSteamNetConnection clientHandle, World* world);

	/// Promote an existing client entry from handle-based to OwnerID-based routing.
	/// Call this after the pump loop once the server has assigned an OwnerID.
	void UpdateClientOwnerID(HSteamNetConnection clientHandle, uint8_t ownerID, World* world);

	void RemoveClient(uint8_t ownerID);
	void ClearClients();

	/// Call after Initialize() to wire the Server child handler to the shared transport.
	/// Must be called before SetServerWorld / AddClient.
	void InitChildren();

	/// Single poll+dispatch cycle for use during synchronous pump loops (PIE startup).
	/// Runs GNS callbacks, drains the incoming message queue, and dispatches each message.
	void PumpMessages();

	ServerNetThread& GetServer() { return Server; }

private:
	void HandleMessage(const ReceivedMessage& msg);
	void TickReplication(); // also ticks all owned FlowManagers
	void TickInputSend();   // delegates to each ClientNetThread handler

	ServerNetThread Server;

	struct ClientEntry
	{
		HSteamNetConnection Handle = 0; // client-side GNS handle — used for routing before OwnerID is assigned
		uint8_t OwnerID            = 0; // 0 = unassigned (handshake not yet complete)
		World* ClientWorld         = nullptr; // non-owning — EditorContext owns the World via FlowManager
		std::unique_ptr<ClientNetThread> Handler;
	};
	std::vector<ClientEntry> Clients;

	uint64_t LastFlowTickTime = 0; // SDL perf counter at last flow tick
};

#endif // TNX_ENABLE_EDITOR
