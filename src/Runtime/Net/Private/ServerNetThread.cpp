#include "ServerNetThread.h"

#include "EngineConfig.h"
#include "FlowManager.h"
#include "NetChannel.h"
#include "NetConnectionManager.h"
#include "NetTypes.h"
#include "RPC.h"
#include "ReplicationSystem.h"
#include "World.h"
#include "Logger.h"

#include <SDL3/SDL_timer.h>
#include <cstring>

void ServerNetThread::BindSoulCallbacks()
{
	if (!ConnectionMgr || !ServerWorld) return;

	ConnectionMgr->OnClientDisconnected.Bind<ServerNetThread, &ServerNetThread::OnClientDisconnectedCB>(this);
}

void ServerNetThread::OnClientDisconnectedCB(uint8_t ownerID)
{
	if (ownerID != 0 && ownerID < MaxOwnerIDs) InputLogs[ownerID].reset(); // free the log; slot becomes nullptr

	if (FlowManager* flow = ServerWorld ? ServerWorld->GetFlowManager() : nullptr)
		if (ownerID != 0) flow->OnClientDisconnected(ownerID);
}

void ServerNetThread::CreateInputLog(uint8_t ownerID)
{
	if (ownerID == 0 || ownerID >= MaxOwnerIDs) return;

	const uint32_t temporalFrameCount = (Config && Config->TemporalFrameCount != EngineConfig::Unset)
											? static_cast<uint32_t>(Config->TemporalFrameCount)
											: 32u;

	InputLogs[ownerID] = std::make_unique<PlayerInputLog>();
	InputLogs[ownerID]->Initialize(temporalFrameCount);
}


void ServerNetThread::WirePlayerInputInjector(World* world)
{
	LogicThread* logic = world ? world->GetLogicThread() : nullptr;
	if (!logic) return;

	// Capture 'this' — ServerNetThread outlives the LogicThread (engine shutdown order).
	// Each sim tick: gate on MaxClientInputLead, consume (real or predicted), inject, check dirty.
	logic->SetPlayerInputInjector([this, world, logic](uint32_t frameNumber)
	{
		const int maxLead = Config ? Config->MaxClientInputLead : 16;

		for (uint32_t ownerID = 1; ownerID < MaxOwnerIDs; ++ownerID)
		{
			PlayerInputLog* log = InputLogs[ownerID].get();
			if (!log) continue;

			// Don't process input until Activate() has been called (PlayerBeginConfirm sent).
			if (!log->bActive) continue;

			InputBuffer* buf = world->GetPlayerSimInput(static_cast<uint8_t>(ownerID));

			// Frame gate: stall if the client is too far behind.
			// Still call Swap() so carry-forward of the last held state is visible via ReadSlot.
			if (maxLead >= 0
				&& frameNumber > log->LastReceivedFrame + static_cast<uint32_t>(maxLead))
			{
				LOG_WARN_F("[ServerNet] Stalling sim for ownerID %u: frame %u, lastReceived %u, lead %d",
						   ownerID, frameNumber, log->LastReceivedFrame, maxLead);
				if (buf) buf->Swap();
				continue;
			}

			InputConsumeResult result = log->ConsumeFrame(frameNumber);
			if (result)
			{
				if (buf)
				{
					buf->InjectState(result.Entry->KeyState,
									 result.Entry->MouseDX,
									 result.Entry->MouseDY,
									 result.Entry->MouseButtons);
					// Mirror into viz slot so PlayerConstruct::ScalarUpdate reads the
					// client's actual mouse look rather than falling back to the server's
					// raw mouse buffer.
					if (InputBuffer* viz = world->GetPlayerVizInput(static_cast<uint8_t>(ownerID)))
					{
						viz->InjectState(result.Entry->KeyState,
										 result.Entry->MouseDX,
										 result.Entry->MouseDY,
										 result.Entry->MouseButtons);
						viz->Swap();
					}
					// TODO: inject discrete events into player event queue
				}

				if (ConnectionInfo* ci = ConnectionMgr ? ConnectionMgr->FindConnectionByOwnerID(static_cast<uint8_t>(ownerID)) : nullptr)
					ci->LastAckedClientFrame = log->LastConsumedFrame;
			}
			else if (result.Reason == InputMissReason::LateOrAliased)
			{
				LOG_WARN_F("[ServerNet] Input data loss for ownerID %u at frame %u", ownerID, frameNumber);
			}
			// NotYetReceived within lead: ConsumeFrame already wrote a predicted entry and returned it.

			// Expose the injected (or predicted/carried-forward) state to the sim this frame.
			if (buf) buf->Swap();

			// Trigger resim if a real packet corrected predicted frames.
			if (log->IsDirty())
			{
				const uint32_t resimFrom = log->EarliestDirtyFrame;
				log->ClearDirty();
				LOG_INFO_F("[ServerNet] Input mismatch for ownerID %u, resim from frame %u", ownerID, resimFrom);
#ifdef TNX_ENABLE_ROLLBACK
				logic->RequestRollback(resimFrom);
#endif
			}
		}
	});
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
			const uint8_t ownerID = msg.Header.SenderID;
			const auto* payload   = reinterpret_cast<const InputFramePayload*>(msg.Payload.data());

			PlayerInputLog* log = GetInputLog(ownerID);
			if (!log)
			{
				LOG_WARN_F("[ServerNet] InputFrame from OwnerID %u — no log (not connected?)", ownerID);
				break;
			}

			// Split payload across per-sim-frame slots in the log. Each frame slot (frame % Depth)
			// mirrors the temporal slab so LogicThread can look up input by frame index directly.
			const uint32_t fixedHz = (Config && Config->FixedUpdateHz != EngineConfig::Unset)
										 ? static_cast<uint32_t>(Config->FixedUpdateHz)
										 : 512u;
			log->Store(*payload, fixedHz);

			// TODO(rollback): if payload->LastClientFrame < serverCurrentFrame (late packet),
			// trigger ExecuteRollback(payload->FirstClientFrame) to resim with corrected input.
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

			if (ServerWorld) ServerWorld->EnsurePlayerInputSlot(ci->OwnerID);

			const uint32_t serverFrame = (ServerWorld && ServerWorld->GetLogicThread())
											 ? ServerWorld->GetLogicThread()->GetLastCompletedFrame()
											 : 0;
			ci->ServerFrameAtHandshake = serverFrame;

			// Create the per-player input log now that we have a stable ownerID.
			// Log is inactive until Activate() is called at PlayerBeginConfirm time.
			CreateInputLog(ci->OwnerID);
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

			const std::string localPath = (ServerWorld && ServerWorld->GetFlowManager())
				? ServerWorld->GetFlowManager()->GetActiveLevelLocalPath() : std::string{};
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

				// ReplicationSystem::SendSpawns Pass 1 will flush the initial entity batch,
				// send ServerReady, and advance RepState → Loaded on the next Tick.
				if (FlowManager* flow = ServerWorld ? ServerWorld->GetFlowManager() : nullptr)
					flow->OnClientLoaded(ci->OwnerID);
			}
			break;
		}

		case NetMessageType::EntityDestroy:
			// TODO
			break;

		case NetMessageType::SoulRPC:
		{
			// All Soul-layer RPCs arrive here. The header identifies the MethodID
			// and ParamSize; FlowManager routes to the correct Soul handler.
			ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
			if (!ci) break;

			if (msg.Payload.size() < sizeof(RPCHeader))
			{
				LOG_WARN_F("[ServerNet] SoulRPC payload too small (%zu bytes)", msg.Payload.size());
				break;
			}

			const auto* rpcHdr = reinterpret_cast<const RPCHeader*>(msg.Payload.data());
			const uint8_t* params = msg.Payload.data() + sizeof(RPCHeader);
			const size_t paramBytes = msg.Payload.size() - sizeof(RPCHeader);

			if (paramBytes < rpcHdr->ParamSize)
			{
				LOG_WARN_F("[ServerNet] SoulRPC param underrun (MethodID=%u, want=%u, got=%zu)",
				           rpcHdr->MethodID, rpcHdr->ParamSize, paramBytes);
				break;
			}

			{
				if (FlowManager* flow = ServerWorld ? ServerWorld->GetFlowManager() : nullptr)
				{
					if (Soul* soul = flow->GetSoul(ci->OwnerID))
					{
						// Refresh the channel so server→client reply RPCs (e.g.
						// PlayerBeginConfirm) have a valid send target. The channel
						// is not set during OnClientLoaded because ConnectionInfo
						// isn't available there.
						soul->GetNetChannel() = NetChannel(ci, ConnectionMgr);
						RPCContext ctx{ci, ConnectionMgr};
						soul->DispatchServerRPC(ctx, *rpcHdr, params);
					}
					else
					{
						LOG_WARN_F("[ServerNet] SoulRPC: no Soul for ownerID=%u (MethodID=%u)",
								   ci->OwnerID, rpcHdr->MethodID);
					}
				}
			}
			break;
		}

		// PlayerBeginRequest (legacy) — superseded by SoulRPC/PlayerBegin. Kept
		// for wire-compat during the transition; can be removed once clients are updated.
		case NetMessageType::PlayerBeginRequest:
			LOG_WARN("[ServerNet] Received legacy PlayerBeginRequest — client should use SoulRPC");
			break;

		default:
			LOG_WARN_F("[ServerNet] Unhandled message type %u", msg.Header.Type);
			break;
	}
}
