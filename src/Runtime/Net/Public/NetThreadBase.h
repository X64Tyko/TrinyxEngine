#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "NetTypes.h"
#include "NetConnectionManager.h"

#include <SDL3/SDL_timer.h>

class GNSContext;
class NetConnectionManager;
class World;
struct EngineConfig;
struct InputBuffer;
struct ReceivedMessage;

// ---------------------------------------------------------------------------
// NetThreadBase<Derived>  (CRTP)
//
// Owns the GNS transport and rate-limited Tick logic. No OS thread — polling
// and rate gating run on the Sentinel thread in TrinyxEngine::RunMainLoop.
//
// Derived must implement two hooks (TickInputSend has a public no-op default):
//   void HandleMessage(const ReceivedMessage& msg)   — role-specific routing
//   void TickReplication()                            — no-op on client
//   void TickInputSend()  [optional override]         — no-op on server; client sends InputFrame
//
// All instances run in the same mode:
//   Sentinel calls PumpMessages() each 1ms tick (Poll + recv + HandleMessage).
//   Sentinel calls TickInputSend() gated at InputNetHz (128Hz).
//   Sentinel calls Tick() gated at NetworkUpdateHz (30Hz) — replication + clock sync.
// ---------------------------------------------------------------------------

template <typename Derived>
class NetThreadBase
{
public:
	NetThreadBase()  = default;
	~NetThreadBase() = default;

	NetThreadBase(const NetThreadBase&)            = delete;
	NetThreadBase& operator=(const NetThreadBase&) = delete;

	void Initialize(GNSContext* gns, const EngineConfig* config);

	// Initialize as a PIE child handler — shares an externally-owned ConnectionMgr.
	void InitAsHandler(GNSContext* gns, const EngineConfig* config, NetConnectionManager* sharedMgr);

	void Tick();

	/// One message-processing iteration: Poll + RunCallbacks + PollIncoming + HandleMessage.
	/// Called from Sentinel on every 1ms tick.
	void PumpMessages();

	/// Convenience alias — equivalent to PumpMessages().
	void PollAndDispatch() { Self().PumpMessages(); }

	NetConnectionManager* GetConnectionManager() { return ConnectionMgr; }
	const NetConnectionManager* GetConnectionManager() const { return ConnectionMgr; }

	// Per-client world routing for input-frame send path.
	// OwnerID 0 = server world. 1-255 = client connections.
	void MapConnectionToWorld(uint8_t ownerID, World* world) { WorldMap[ownerID] = world; }

	// Default no-op — server inherits this; client and PIE override.
	void TickInputSend()
	{
	}

protected:
	Derived& Self() { return *static_cast<Derived*>(this); }

	GNSContext* GNS            = nullptr;
	const EngineConfig* Config = nullptr;

	// Owning pointer — only set when this instance called Initialize() (not InitAsHandler()).
	std::unique_ptr<NetConnectionManager> OwnedConnectionMgr;
	// Raw view — always valid after Initialize() or InitAsHandler().
	NetConnectionManager* ConnectionMgr = nullptr;

	// OwnerID → World for the input-send path (client legs only)
	World* WorldMap[MaxOwnerIDs]{};

private:
	void TickClockSync(double nowSec);
};

// ---------------------------------------------------------------------------
// Implementation (header-only so template is visible to all translation units)
// ---------------------------------------------------------------------------

#include "GNSContext.h"
#include "EngineConfig.h"
#include "World.h"
#include "Input.h"
#include "LogicThread.h"
#include "Logger.h"

template <typename Derived>
void NetThreadBase<Derived>::Initialize(GNSContext* gns, const EngineConfig* config)
{
	GNS    = gns;
	Config = config;

	OwnedConnectionMgr = std::make_unique<NetConnectionManager>();
	OwnedConnectionMgr->Initialize(gns);
	OwnedConnectionMgr->SetNoNagle(config && config->NoNagle);
	if (config && (config->SendRateMin != EngineConfig::Unset || config->SendRateMax != EngineConfig::Unset)) OwnedConnectionMgr->SetSendRate(config->SendRateMin, config->SendRateMax);
	ConnectionMgr = OwnedConnectionMgr.get();
}

template <typename Derived>
void NetThreadBase<Derived>::InitAsHandler(GNSContext* gns, const EngineConfig* config, NetConnectionManager* sharedMgr)
{
	GNS           = gns;
	Config        = config;
	ConnectionMgr = sharedMgr; // non-owning
}

template <typename Derived>
void NetThreadBase<Derived>::Tick()
{
	TNX_ZONE_N("Net_Tick");
	Self().TickReplication();

	const double nowSec = static_cast<double>(SDL_GetPerformanceCounter())
		/ static_cast<double>(SDL_GetPerformanceFrequency());
	TickClockSync(nowSec);
}

template <typename Derived>
void NetThreadBase<Derived>::PumpMessages()
{
	GNS->Poll();
	ConnectionMgr->RunCallbacks();

	std::vector<ReceivedMessage> messages;
	ConnectionMgr->PollIncoming(messages);
	for (const auto& msg : messages) Self().HandleMessage(msg);
}

template <typename Derived>
void NetThreadBase<Derived>::TickClockSync(double nowSec)
{
	const uint8_t probeTarget = static_cast<uint8_t>(
		(Config->ClockSyncProbes == EngineConfig::Unset) ? 8 : Config->ClockSyncProbes);

	std::vector<HSteamNetConnection> handles;
	for (const auto& ci : ConnectionMgr->GetConnections())
		if (ci.bConnected && ci.bClientInitiated)
			handles.push_back(ci.Handle);

	for (HSteamNetConnection handle : handles)
	{
		ConnectionInfo* ci = ConnectionMgr->FindConnection(handle);
		if (!ci) continue;

		if (ci->RepState == ClientRepState::Synchronizing)
		{
			if (ci->ClockSyncProbesSent < probeTarget)
			{
				PacketHeader ping{};
				ping.Type        = static_cast<uint8_t>(NetMessageType::Ping);
				ping.Flags       = PacketFlag::DefaultFlags;
				ping.SequenceNum = ci->NextSeqOut++;
				ping.Timestamp   = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
				ConnectionMgr->Send(handle, ping, nullptr, false);
				ci->ClockSyncProbesSent++;
				ci->LastHeartbeatTime = nowSec;
			}
			else if (ci->ClockSyncProbesSent < 255 && ci->ClockSyncProbesRecvd >= probeTarget)
			{
				ClockSyncPayload csReq{};
				csReq.ClientTimestamp       = SDL_GetPerformanceCounter();
				csReq.ServerFrame           = 0;
				csReq.LocalFrameAtHandshake = ci->ClientLocalFrameAtHandshake;

				PacketHeader header{};
				header.Type        = static_cast<uint8_t>(NetMessageType::ClockSync);
				header.Flags       = PacketFlag::DefaultFlags;
				header.SequenceNum = ci->NextSeqOut++;
				header.Timestamp   = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
				header.PayloadSize = sizeof(ClockSyncPayload);
				ConnectionMgr->Send(handle, header,
									reinterpret_cast<const uint8_t*>(&csReq), false);
				ci->ClockSyncProbesSent = 255;
				ci->LastHeartbeatTime   = nowSec;
			}
		}

		if (ci->RepState >= ClientRepState::Synchronizing
			&& ci->ClockSyncProbesSent >= probeTarget
			&& (nowSec - ci->LastHeartbeatTime) >= 1.0)
		{
			PacketHeader ping{};
			ping.Type        = static_cast<uint8_t>(NetMessageType::Ping);
			ping.Flags       = PacketFlag::DefaultFlags;
			ping.SequenceNum = ci->NextSeqOut++;
			ping.Timestamp   = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
			ConnectionMgr->Send(handle, ping, nullptr, false);
			ci->LastHeartbeatTime = nowSec;
		}
	}
}
