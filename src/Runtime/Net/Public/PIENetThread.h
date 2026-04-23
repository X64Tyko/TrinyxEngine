#pragma once
#ifdef TNX_ENABLE_EDITOR

#include "NetThreadBase.h"
#include "AuthorityNet.h"
#include "OwnerNet.h"

#include <vector>

class ReplicationSystem;
class WorldBase;

// ---------------------------------------------------------------------------
// PIENetThread
//
// PIE coordinator. Owns a single GNS transport (shared with children) and
// is driven by the Sentinel thread. On each PumpMessages() call it routes
// messages by bAuthoritySide to the appropriate child handler. Children are pure
// message-handlers — they do NOT have their own threads and their Initialize()
// is not called (they share this instance's ConnectionMgr).
//
// FlowManager is always resolved from world->GetFlowManager() — no separate
// flow pointers needed. Each World knows its owning FlowManager.
//
// Usage:
//   pie.Initialize(gns, config);
//   pie.SetAuthorityWorld(serverWorld);
//   pie.SetReplicationSystem(repl);
//   pie.AddClient(ownerID, clientWorld);
//   // Driven by Sentinel: PumpMessages(), TickInputSend(), Tick()
// ---------------------------------------------------------------------------
class PIENetThread : public NetThreadBase<PIENetThread>
{
	friend class NetThreadBase<PIENetThread>;

public:
	void SetAuthorityWorld(WorldBase* world);
	void SetReplicationSystem(ReplicationSystem* repl);

	/// Register a client handler keyed by its GNS connection handle.
	/// Call this immediately after Connect(), before any PumpMessages() calls,
	/// so the handler is in place to receive the handshake reply.
	void AddClient(HSteamNetConnection clientHandle, WorldBase* world);

	/// Promote an existing client entry from handle-based to OwnerID-based routing.
	/// Call this after a PumpMessages() cycle once the server has assigned an OwnerID.
	void UpdateClientOwnerID(HSteamNetConnection clientHandle, uint8_t ownerID, WorldBase* world);

	void RemoveClient(uint8_t ownerID);
	void ClearClients();

	/// Call after Initialize() to wire the Server child handler to the shared transport.
	/// Must be called before SetAuthorityWorld / AddClient.
	void InitChildren();

	AuthorityNet& GetAuthority() { return Authority; }

	// Called by Sentinel — must be public.
	void TickInputSend(); // delegates to each OwnerNet handler
	void TickDispatch();  // delegates to Authority

private:
	void HandleMessage(const ReceivedMessage& msg);
	void TickReplication(); // also ticks all owned FlowManagers

	AuthorityNet Authority;

	struct ClientEntry
	{
		HSteamNetConnection Handle = 0; // client-side GNS handle — used for routing before OwnerID is assigned
		uint8_t OwnerID            = 0; // 0 = unassigned (handshake not yet complete)
		WorldBase* OwnerWorld      = nullptr; // non-owning — EditorContext owns the World via FlowManager
		std::unique_ptr<OwnerNet> Handler;
	};
	std::vector<ClientEntry> Clients;

	uint64_t LastFlowTickTime = 0; // SDL perf counter at last flow tick
};

#endif // TNX_ENABLE_EDITOR
