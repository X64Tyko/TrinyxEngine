#pragma once
#include "NetThreadBase.h"
#include "PlayerInputLog.h"
#include "RegistryTypes.h"
#include <array>
#include <memory>

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
// FlowManager is resolved from ServerWorld->GetFlowManager() — no separate
// FlowMgr pointer needed.
// ---------------------------------------------------------------------------
class ServerNetThread : public NetThreadBase<ServerNetThread>
{
	friend class NetThreadBase<ServerNetThread>;

public:
	/// Non-owning. Set before Start() / first Tick().
	void SetServerWorld(World* world) { ServerWorld = world; }
	World* GetServerWorld() const { return ServerWorld; }

	void SetReplicationSystem(ReplicationSystem* repl) { Replicator = repl; }

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

	/// Creates a PlayerInputLog for ownerID, sized to match the temporal slab.
	/// Called from the ConnectionHandshake handler when an ownerID is assigned.
	/// Exposed publicly so tests and editor tooling can wire a player log manually
	/// when bypassing the full handshake flow (e.g. via direct AssignOwnerID).
	void CreateInputLog(uint8_t ownerID);

private:
	void OnClientDisconnectedCB(uint8_t ownerID);

	ReplicationSystem* Replicator = nullptr;
	World* ServerWorld            = nullptr;

	// One log per ownerID slot — only allocated for connected players.
	// Slot 0 (server) is never populated. Depth == TemporalFrameCount.
	std::array<std::unique_ptr<PlayerInputLog>, MaxOwnerIDs> InputLogs{};

	// Coalesced input-mismatch rollback target. Accumulated on the LogicThread by the
	// injector lambda whenever a real packet corrects a predicted frame. Fired at the
	// start of the next non-resim injection pass so that all dirty marks from the burst
	// are folded into one rollback instead of one per frame.
	uint32_t PendingInputResimFrame = UINT32_MAX;
};
