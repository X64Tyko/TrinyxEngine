#include "NetThreadBase.h"

#include "AuthorityNet.h"
#include "OwnerNet.h"
#ifdef TNX_ENABLE_EDITOR
#include "PIENetThread.h"
#endif

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
	if (!ConnectionMgr) return;

	GNS->Poll();

	ConnectionMgr->RunCallbacks();

	std::vector<ReceivedMessage> messages;
	ConnectionMgr->PollIncoming(messages);
	for (const auto& msg : messages)
		Self().HandleMessage(msg);
}

template <typename Derived>
void NetThreadBase<Derived>::TickClockSync(double nowSec)
{
	const uint8_t probeTarget = static_cast<uint8_t>(
		(Config->ClockSyncProbes == EngineConfig::Unset) ? 8 : Config->ClockSyncProbes);

	std::vector<HSteamNetConnection> handles;
	for (const auto& ci : ConnectionMgr->GetConnections())
		if (ci.bConnected && ci.bOwnerInitiated)
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

// ---------------------------------------------------------------------------
// Explicit instantiations — the closed set of derived net handlers.
// ---------------------------------------------------------------------------
template class NetThreadBase<AuthorityNet>;
template class NetThreadBase<OwnerNet>;
#ifdef TNX_ENABLE_EDITOR
template class NetThreadBase<PIENetThread>;
#endif
