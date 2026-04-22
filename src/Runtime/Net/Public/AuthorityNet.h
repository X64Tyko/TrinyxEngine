#pragma once
#include "NetThreadBase.h"
#include "PlayerInputLog.h"
#include "ReplicationSystem.h"
#include "RegistryTypes.h"
#include "AuthoritySim.h"
#include <memory>

class WorldBase;
class LogicThreadBase;

// ---------------------------------------------------------------------------
// AuthorityNet
//
// Handles all server-side message routing:
//   ConnectionHandshake  InputFrame  Ping/Pong
//   ClockSync + TravelNotify  LevelReady  SoulRPC
//
// Owns the server world pointer (non-owning ref) and replication system
// (also non-owning — caller manages lifetime).
// ---------------------------------------------------------------------------
class AuthorityNet : public NetThreadBase<AuthorityNet>
{
friend class NetThreadBase<AuthorityNet>;

public:
void SetAuthorityWorld(WorldBase* world) { AuthorityWorld = world; }
WorldBase* GetAuthorityWorld() const { return AuthorityWorld; }

void SetReplicationSystem(ReplicationSystem* repl) { Replicator = repl; }
ReplicationSystem* GetReplicator() const { return Replicator; }
const EngineConfig* GetConfig() const { return Config; }

/// Called after ConnectionMgr is valid (post Initialize/InitAsHandler).
void BindSoulCallbacks();

/// Returns the input log for ownerID if the channel is active, nullptr otherwise.
PlayerInputLog* GetInputLog(uint8_t ownerID)
{
if (!Replicator) return nullptr;
ServerClientChannel* ch = Replicator->GetChannelIfActive(ownerID);
return ch ? &ch->InputLog : nullptr;
}

/// Wire AuthoritySim into the world's LogicThread as the active net mode.
/// Call after both LogicThread and AuthorityNet are initialized.
void WireNetMode(WorldBase* world);

void HandleMessage(const ReceivedMessage& msg);
void TickReplication();

/// Opens a ServerClientChannel for ownerID, sized to match the temporal slab.
/// Call within the ConnectionHandshake handler when an ownerID is assigned.
void CreateInputLog(uint8_t ownerID);

private:
void OnClientDisconnectedCB(uint8_t ownerID);

ReplicationSystem* Replicator    = nullptr;
WorldBase*         AuthorityWorld = nullptr;

// The active net mode instance — wired into LogicThread by WireNetMode.
AuthoritySim NetMode;
};
