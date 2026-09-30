#pragma once
#include "NetThreadBase.h"
#include "PlayerInputLog.h"
#include "ReplicationSystem.h"
#include "RegistryTypes.h"
#include "AuthoritySim.h"
#include <memory>

class WorldBase;
class LogicThreadBase;

/// @brief Authority-side network handler. Owns server-side message routing.
///
/// Routes inbound messages: @c ConnectionHandshake, @c InputFrame, @c Ping/Pong,
/// @c ClockSync, @c TravelNotify, @c LevelReady, @c SoulRPC.
///
/// Holds non-owning pointers to the server World and @ref ReplicationSystem —
/// the caller (typically @c TrinyxEngine) manages their lifetimes.
class AuthorityNet : public NetThreadBase<AuthorityNet>
{
	friend class NetThreadBase<AuthorityNet>;

public:
	void SetAuthorityWorld(WorldBase* world) { AuthorityWorld = world; } ///< Set the authority-side world (non-owning).
	WorldBase* GetAuthorityWorld() const { return AuthorityWorld; }      ///< Get the authority-side world.

	void SetReplicationSystem(ReplicationSystem* repl) { Replicator = repl; } ///< Bind the @ref ReplicationSystem (non-owning).
	ReplicationSystem* GetReplicator() const { return Replicator; }           ///< Get the bound @ref ReplicationSystem.
	const EngineConfig* GetConfig() const { return Config; }                  ///< Get the engine configuration.

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

	void HandleMessage(const ReceivedMessage& msg); ///< Route one inbound network message.

	/// @brief Dispatch spawn and correction build jobs for newly published logic frames.
	/// Called from Sentinel on every loop tick (ungated).
	void TickDispatch();

	/// @brief Flush built packets to the wire at the network update rate.
	void TickReplication();

	/// Opens a ServerClientChannel for ownerID, sized to match the temporal slab.
	/// Call within the ConnectionHandshake handler when an ownerID is assigned.
	void CreateInputLog(uint8_t ownerID);

	/// Tell ownerID's client to background-load a content chunk.
	/// bAutoActivate=true: client activates immediately on load (fire-and-forget, no reply).
	/// bAutoActivate=false: client sends StreamReady; server drives activation via SendChunkActivate.
	void SendStreamLoad(uint8_t ownerID, int64_t assetIDRaw, uint16_t instanceIndex, bool bAutoActivate);

	/// Tell ownerID's client to activate a previously streamed chunk.
	/// Only meaningful when the original StreamLoad had bAutoActivate=false.
	void SendChunkActivate(uint8_t ownerID, int64_t assetIDRaw, uint16_t instanceIndex);

private:
	void OnClientDisconnectedCB(uint8_t ownerID);

	ReplicationSystem* Replicator = nullptr;
	WorldBase* AuthorityWorld     = nullptr;

	// The active net mode instance — wired into LogicThread by WireNetMode.
	AuthoritySim NetMode;
};
