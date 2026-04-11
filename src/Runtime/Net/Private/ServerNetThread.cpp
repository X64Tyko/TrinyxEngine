#include "ServerNetThread.h"

#include "FlowManager.h"
#include "NetChannel.h"
#include "NetConnectionManager.h"
#include "NetTypes.h"
#include "ReplicationSystem.h"
#include "World.h"
#include "Logger.h"

#include <SDL3/SDL_timer.h>
#include <cstring>

void ServerNetThread::SetFlowManager(FlowManager* flow)
{
	FlowMgr = flow;
}

void ServerNetThread::BindSoulCallbacks()
{
	if (!ConnectionMgr || !FlowMgr) return;

	ConnectionMgr->OnClientDisconnected.Bind<ServerNetThread, &ServerNetThread::OnClientDisconnectedCB>(this);
}

void ServerNetThread::OnClientDisconnectedCB(uint8_t ownerID)
{
	if (FlowMgr&& ownerID 
	!=
	0
	)
	FlowMgr->OnClientDisconnected(ownerID);
}

void ServerNetThread::TickReplication()
{
	if (Replicator) Replicator->Tick(ConnectionMgr);
}

void ServerNetThread::HandleMessage(const ReceivedMessage& msg)
{
	auto type = static_cast<NetMessageType>(msg.Header.Type);

	switch (type)
	{
		case NetMessageType::InputFrame:
		{
			if (!ServerWorld)
			{
				LOG_WARN("[ServerNet] InputFrame received but ServerWorld is null — dropped");
				break;
			}
			if (msg.Payload.size() < sizeof(InputFramePayload))
			{
				LOG_WARN_F("[ServerNet] InputFrame payload too small (%zu)", msg.Payload.size());
				break;
			}
			const uint8_t ownerID  = msg.Header.SenderID;
			const auto* payload    = reinterpret_cast<const InputFramePayload*>(msg.Payload.data());

			ServerWorld->EnsurePlayerInputSlot(ownerID);
			InputBuffer* simInput = ServerWorld->GetPlayerSimInput(ownerID);
			InputBuffer* vizInput = ServerWorld->GetPlayerVizInput(ownerID);
			if (!simInput)
			{
				LOG_WARN_F("[ServerNet] InputFrame from OwnerID %u out of range — dropped", ownerID);
				break;
			}
			simInput->InjectState(payload->KeyState, payload->MouseDX, payload->MouseDY, payload->MouseButtons);
			vizInput->InjectState(payload->KeyState, payload->MouseDX, payload->MouseDY, payload->MouseButtons);
			break;
		}

		case NetMessageType::Ping:
		{
			ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
			if (ci) NetChannel(ci, ConnectionMgr).SendPong(msg.Header);
			break;
		}

		case NetMessageType::Pong:
		{
			ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
			if (ci)
			{
				const uint16_t now  = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
				const uint16_t sent = msg.Header.Timestamp;
				const float rtt     = static_cast<float>(static_cast<uint16_t>(now - sent));
				if (ci->RTT_ms <= 0.0f) ci->RTT_ms = rtt;
				else ci->RTT_ms                    = ci->RTT_ms * 0.875f + rtt * 0.125f;
			}
			break;
		}

		case NetMessageType::ConnectionHandshake:
		{
			ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
			if (!ci)
			{
				LOG_WARN_F("[ServerNet] ConnectionHandshake from unknown connection %u", msg.Connection);
				break;
			}

			if (msg.Header.SenderID != 0)
			{
				LOG_WARN_F("[ServerNet] ConnectionHandshake from client with existing ownerID %u", msg.Header.SenderID);
				break;
			}

			ConnectionMgr->GenerateNetID(msg.Connection);

			const uint32_t serverFrame = (ServerWorld && ServerWorld->GetLogicThread())
											 ? ServerWorld->GetLogicThread()->GetLastCompletedFrame()
											 : 0;
			ci->ServerFrameAtHandshake = serverFrame;
			ci->RepState               = ClientRepState::Synchronizing;

			HandshakePayload hsPay{};
			hsPay.TickRate = static_cast<uint32_t>(
				Config->FixedUpdateHz == EngineConfig::Unset ? 128 : Config->FixedUpdateHz);
			hsPay.ServerFrame = serverFrame;

			NetChannel(ci, ConnectionMgr).Send(
				NetMessageType::ConnectionHandshake, hsPay, /*reliable=*/true, serverFrame);

			LOG_INFO_F("[ServerNet] HandshakeAccept → OwnerID=%u frame=%u tickRate=%u",
					   ci->OwnerID, serverFrame, hsPay.TickRate);
			break;
		}

		case NetMessageType::ClockSync:
		{
			ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
			if (!ci) break;

			if (msg.Payload.size() < sizeof(ClockSyncPayload))
			{
				LOG_WARN_F("[ServerNet] ClockSync payload too small (%zu)", msg.Payload.size());
				break;
			}

			const auto* req = reinterpret_cast<const ClockSyncPayload*>(msg.Payload.data());

			const uint32_t serverFrame = (ServerWorld && ServerWorld->GetLogicThread())
											 ? ServerWorld->GetLogicThread()->GetLastCompletedFrame()
											 : 0;

			ClockSyncPayload resp{};
			resp.ClientTimestamp = req->ClientTimestamp;
			resp.ServerFrame     = serverFrame;

			NetChannel ch(ci, ConnectionMgr);
			ch.Send(NetMessageType::ClockSync, resp, /*reliable=*/false, serverFrame);

			const std::string localPath = FlowMgr ? FlowMgr->GetActiveLevelLocalPath() : std::string{};
			if (!localPath.empty())
			{
				TravelPayload travelMsg{};
				travelMsg.PathLength = static_cast<uint8_t>(std::min(localPath.size(), size_t(254)));
				if (localPath.size() > 254)
					LOG_WARN_F("[ServerNet] Level path truncated: %s", localPath.c_str());
				std::memcpy(travelMsg.LevelPath, localPath.c_str(), travelMsg.PathLength);
				travelMsg.LevelPath[travelMsg.PathLength] = '\0';

				ch.Send(NetMessageType::TravelNotify, travelMsg, /*reliable=*/true, serverFrame);

				ci->RepState = ClientRepState::LevelLoading;
				LOG_INFO_F("[ServerNet] ClockSyncResponse + TravelNotify → LevelLoading (frame=%u, level=%s)",
						   serverFrame, travelMsg.LevelPath);
			}
			else
			{
				ci->RepState = ClientRepState::Loading;
				LOG_INFO_F("[ServerNet] ClockSyncResponse → Loading (frame=%u) [no level loaded]", serverFrame);
			}
			break;
		}

		case NetMessageType::LevelReady:
		{
			ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
			if (!ci) break;

			if (ci->RepState == ClientRepState::LevelLoading)
			{
				ci->RepState = ClientRepState::LevelLoaded;
				LOG_INFO_F("[ServerNet] LevelReady received — client LevelLoaded (ownerID=%u)", ci->OwnerID);

				// TODO: flush initial entity batch to this client (EntitySpawn with Alive flag)
				// For now advance immediately to Loaded and send ServerReady.

				ci->RepState = ClientRepState::Loaded;

				FlowEventPayload ready{};
				ready.EventID = static_cast<uint8_t>(FlowEventID::ServerReady);
				NetChannel(ci, ConnectionMgr).Send(
					NetMessageType::FlowEvent, ready, /*reliable=*/true, msg.Header.FrameNumber);

				LOG_INFO_F("[ServerNet] ServerReady sent (ownerID=%u)", ci->OwnerID);

				if (FlowMgr) FlowMgr->OnClientLoaded(ci->OwnerID);
			}
			break;
		}

		case NetMessageType::EntityDestroy:
			// TODO
			break;

		default:
			LOG_WARN_F("[ServerNet] Unhandled message type %u", msg.Header.Type);
			break;
	}
}
