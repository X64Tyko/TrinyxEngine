#pragma once
#include "NetThreadBase.h"
#include "PlayerInputLog.h"
#include "ReplicationSystem.h"
#include "RegistryTypes.h"
#include <memory>

class World;

// ---------------------------------------------------------------------------
// AuthorityNet
//
// Handles all server-side message routing:
//   ConnectionHandshake (server accept)  InputFrame  Ping/Pong
//   ClockSync (server respond + TravelNotify)  LevelReady
//
// Owns the server world pointer (non-owning ref) and replication system
// (also non-owning — caller manages lifetime).
//
// FlowManager is resolved from AuthorityWorld->GetFlowManager() — no separate
// FlowMgr pointer needed.
// ---------------------------------------------------------------------------
class AuthorityNet : public NetThreadBase<AuthorityNet>
{
	friend class NetThreadBase<AuthorityNet>;

public:
	void SetAuthorityWorld(World* world) { AuthorityWorld = world; }
	World* GetAuthorityWorld() const { return AuthorityWorld; }

	void SetReplicationSystem(ReplicationSystem* repl) { Replicator = repl; }

	/// Called after ConnectionMgr is valid (post Initialize/InitAsHandler).
	void BindSoulCallbacks();

	/// Returns the input log for ownerID if the channel is active, nullptr otherwise.
	PlayerInputLog* GetInputLog(uint8_t ownerID)
	{
		if (!Replicator) return nullptr;
		ServerClientChannel* ch = Replicator->GetChannelIfActive(ownerID);
		return ch ? &ch->InputLog : nullptr;
	}

	/// Wire the per-player input injector into the world's LogicThread.
	void WirePlayerInputInjector(World* world);

	void HandleMessage(const ReceivedMessage& msg);
	void TickReplication();

	/// Opens a ServerClientChannel for ownerID, sized to match the temporal slab.
	/// Call within the ConnectionHandshake handler when an ownerID is assigned.
	void CreateInputLog(uint8_t ownerID);

private:
	void OnClientDisconnectedCB(uint8_t ownerID);

	ReplicationSystem* Replicator = nullptr;
	World* AuthorityWorld         = nullptr;

	// Coalesced input-mismatch rollback target. Accumulated on the LogicThread by the
	// injector lambda; fired at the start of the next non-resim pass as one consolidated rollback.
	uint32_t PendingInputResimFrame = UINT32_MAX;
};
