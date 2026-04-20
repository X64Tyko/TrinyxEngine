#include "ClientNetThread.h"

#include "CacheSlotMeta.h"
#include "ConstructRegistry.h"
#include "EngineConfig.h"
#include "FlowManager.h"
#include "FlowState.h"
#include "Input.h"
#include "LogicThread.h"
#include "NetChannel.h"
#include "NetConnectionManager.h"
#include "NetTypes.h"
#include "RPC.h"
#include "Registry.h"
#include "ReplicationSystem.h"
#include "TemporalComponentCache.h"
#include "World.h"
#include "Logger.h"

#include <SDL3/SDL_timer.h>

void ClientNetThread::HandleMessage(const ReceivedMessage& msg)
{
	// Every server-sent header carries the last client input frame it consumed.
	// Advance LastServerAckedFrame so TickInputSend() widens from there, not from last send.
	if (msg.Header.AckedClientFrame > 0)
	{
		if (ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection))
		{
			if (msg.Header.AckedClientFrame > ci->LastServerAckedFrame)
			{
				ci->LastServerAckedFrame = msg.Header.AckedClientFrame;
			}
		}
	}

	auto type = static_cast<NetMessageType>(msg.Header.Type);

	switch (type)
	{
		case NetMessageType::InputFrame:
			{
				//LOG_ENG_INFO_F("new Input Ack Floor: %u", msg.Header.AckedClientFrame);
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

					if (ci->bClientInitiated
						&& ci->RepState == ClientRepState::Synchronizing
						&& ci->ClockSyncProbesRecvd < 8)
					{
						ci->ClockSyncProbesRecvd++;
					}
				}
				break;
			}

		case NetMessageType::ConnectionHandshake:
			{
				ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
				if (!ci)
				{
					LOG_ENG_WARN_F("[ClientNet] ConnectionHandshake from unknown connection %u", msg.Connection);
					break;
				}

				if (msg.Header.SenderID == 0)
				{
					LOG_ENG_WARN("[ClientNet] Invalid HandshakeAccept — SenderID is 0");
					break;
				}

				ConnectionMgr->AssignOwnerID(msg.Connection, msg.Header.SenderID);

				// Promote the world from the temporary slot-0 registration (set in
				// PIENetThread::AddClient) to the real ownerID slot so subsequent handlers
				// (TravelNotify, FlowEvent/ServerReady) can find it before
				// PIENetThread::UpdateClientOwnerID is called from the startup loop.
				if (WorldMap[msg.Header.SenderID] == nullptr && WorldMap[0] != nullptr) WorldMap[msg.Header.SenderID] = WorldMap[0];

				if (msg.Payload.size() >= sizeof(HandshakePayload))
				{
					const auto* hsPay          = reinterpret_cast<const HandshakePayload*>(msg.Payload.data());
					ci->ServerFrameAtHandshake = hsPay->ServerFrame;

					// Record our own frame so we can translate local frame numbers to
					// server-relative frames when building InputFrame packets.
					World* w                        = WorldMap[msg.Header.SenderID];
					ci->ClientLocalFrameAtHandshake = (w && w->GetLogicThread())
														  ? w->GetLogicThread()->GetLastCompletedFrame()
														  : 0;
					if (w) w->SetServerFrameOffset(ci->GetFrameOffset());
				}
				ci->RepState = ClientRepState::Synchronizing;

				{
					World* origWorld = WorldMap[msg.Header.SenderID];
					if (origWorld && origWorld->GetFlowManager()) origWorld->GetFlowManager()->OnLocalOwnerConnected(msg.Header.SenderID);
					Soul* soul = (origWorld && origWorld->GetFlowManager()) ? origWorld->GetFlowManager()->GetSoul(msg.Header.SenderID) : nullptr;
					LOG_NET_INFO_F(soul, "[ClientNet] HandshakeAccept received — OwnerID=%u serverFrame=%u",
								   msg.Header.SenderID, ci->ServerFrameAtHandshake);
				}
				break;
			}

		case NetMessageType::ClockSync:
			{
				ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
				if (!ci) break;

				if (msg.Payload.size() < sizeof(ClockSyncPayload))
				{
					LOG_ENG_WARN_F("[ClientNet] ClockSync payload too small (%zu)", msg.Payload.size());
					break;
				}

				const uint32_t tickRate = (Config->FixedUpdateHz == EngineConfig::Unset)
											  ? 128u
											  : static_cast<uint32_t>(Config->FixedUpdateHz);
				const float stepMs = 1000.0f / static_cast<float>(tickRate);
				// InputLead is kept on ConnectionInfo for diagnostics but is no longer used
				// to offset packet frame tags — the logic thread stamps each frame exactly.
				ci->InputLead = static_cast<uint32_t>(ci->RTT_ms * 0.5f / stepMs) + 2u;

				// Guard: ClockSync (unreliable) can arrive after TravelNotify (reliable).
				if (ci->RepState == ClientRepState::Synchronizing)
				{
					ci->RepState     = ClientRepState::Loading;
					World* origWorld = WorldMap[ci->OwnerID];
					Soul* soul       = (origWorld && origWorld->GetFlowManager()) ? origWorld->GetFlowManager()->GetSoul(ci->OwnerID) : nullptr;
					LOG_NET_INFO_F(soul, "[ClientNet] ClockSync complete — InputLead=%u RTT=%.1fms → Loading",
								   ci->InputLead, ci->RTT_ms);
				}
				else
				{
					World* origWorld = WorldMap[ci->OwnerID];
					Soul* soul       = (origWorld && origWorld->GetFlowManager()) ? origWorld->GetFlowManager()->GetSoul(ci->OwnerID) : nullptr;
					LOG_NET_WARN_F(soul,
								   "[ClientNet] ClockSync arrived late (RepState=%d, already past Synchronizing) — "
								   "InputLead=%u updated, state unchanged",
								   static_cast<int>(ci->RepState), ci->InputLead);
				}
				break;
			}

		case NetMessageType::TravelNotify:
			{
				ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
				if (!ci) break;

				if (msg.Payload.size() < sizeof(TravelPayload))
				{
					LOG_ENG_WARN_F("[ClientNet] TravelNotify payload too small (%zu)", msg.Payload.size());
					break;
				}

				const auto* travelMsg = reinterpret_cast<const TravelPayload*>(msg.Payload.data());
				ci->RepState          = ClientRepState::LevelLoading;

				{
					World* clientWorld = WorldMap[ci->OwnerID];
					FlowManager* flow  = clientWorld ? clientWorld->GetFlowManager() : nullptr;
					Soul* soul         = flow ? flow->GetSoul(ci->OwnerID) : nullptr;
					LOG_NET_INFO_F(soul, "[ClientNet] TravelNotify received — loading level '%s'", travelMsg->LevelPath);
					if (flow) flow->PostTravelNotify(travelMsg->LevelPath);
				}

				// Auto-acknowledge (synchronous load in PIE).
				// Future: remove and let FlowState call AcknowledgeLevelReady() after async load.
				NetChannel(ci, ConnectionMgr).SendHeaderOnly(NetMessageType::LevelReady, /*reliable=*/true);

				ci->RepState = ClientRepState::LevelLoaded;
				{
					World* clientWorld = WorldMap[ci->OwnerID];
					FlowManager* flow  = clientWorld ? clientWorld->GetFlowManager() : nullptr;
					Soul* soul         = flow ? flow->GetSoul(ci->OwnerID) : nullptr;
					LOG_NET_INFO(soul, "[ClientNet] LevelReady sent → client LevelLoaded");
				}
				break;
			}

		case NetMessageType::FlowEvent:
			{
				if (msg.Payload.size() < sizeof(FlowEventPayload))
				{
					LOG_ENG_WARN_F("[ClientNet] FlowEvent payload too small (%zu)", msg.Payload.size());
					break;
				}

				const auto* ev     = reinterpret_cast<const FlowEventPayload*>(msg.Payload.data());
				ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
				if (!ci) break;

				{
					World* clientWorld = WorldMap[ci->OwnerID];
					FlowManager* flow  = clientWorld ? clientWorld->GetFlowManager() : nullptr;
					Soul* soul         = flow ? flow->GetSoul(ci->OwnerID) : nullptr;

					if (ci->RepState == ClientRepState::LevelLoaded
						&& ev->EventID == static_cast<uint8_t>(FlowEventID::ServerReady))
					{
						ci->RepState = ClientRepState::Loaded;

						const bool bSweep = flow && flow->GetActiveState()
							&& flow->GetActiveState()->GetRequirements().SweepsAliveFlagsOnServerReady;
						if (bSweep && clientWorld)
						{
							Registry* reg   = clientWorld->GetRegistry();
							Soul* sweepSoul = soul;
							clientWorld->PostAndWait([reg, sweepSoul](uint32_t frame)
							{
								int count = reg->SweepAliveFlagsToActive();
								LOG_NET_INFO_F(sweepSoul, "[Replication] ServerReady: swept %d Alive→Active", count);
#ifdef TNX_ENABLE_ROLLBACK
								reg->PushServerEvent({frame, [reg]() { reg->SweepAliveFlagsToActive(); }});
#else
								(void)frame;
#endif
							});
						}

						if (flow) flow->SendPlayerBeginRequest(NetChannel(ci, ConnectionMgr), msg.Header.FrameNumber, ci->Predictions);
						ci->PlayerBeginSentAt = SDL_GetTicks();

						LOG_NET_INFO(soul, "[ClientNet] FlowEvent::ServerReady → Loaded");
					}
					else if (ev->EventID == static_cast<uint8_t>(FlowEventID::ServerReady))
					{
						LOG_NET_WARN_F(soul,
									   "[ClientNet] FlowEvent::ServerReady in unexpected RepState=%d "
									   "(expected LevelLoaded=%d) -- player begin skipped",
									   static_cast<int>(ci->RepState),
									   static_cast<int>(ClientRepState::LevelLoaded));
					}

					if (flow) flow->PostNetEvent(ev->EventID);
				}
				break;
			}

		case NetMessageType::EntitySpawn:
			{
				if (msg.Payload.size() < sizeof(EntitySpawnPayload))
				{
					LOG_ENG_WARN_F("[ClientNet] EntitySpawn payload too small (%zu)", msg.Payload.size());
					break;
				}

				ConnectionInfo* ci    = ConnectionMgr->FindConnection(msg.Connection);
				const uint8_t ownerID = ci ? ci->OwnerID : 0;
				if (!WorldMap[ownerID])
				{
					LOG_ENG_WARN_F("[ClientNet] EntitySpawn but no client world for OwnerID %u", ownerID);
					break;
				}

				// Defer to TickReplication — never block the fast-path message loop.
				DeferredEntitySpawn deferred{ownerID, msg.Header.FrameNumber, msg.Payload};
				DeferredEntitySpawns.push_back(std::move(deferred));
				break;
			}

		case NetMessageType::ConstructSpawn:
			{
				if (msg.Payload.size() < sizeof(ConstructSpawnPayload))
				{
					LOG_ENG_WARN_F("[ClientNet] ConstructSpawn payload too small (%zu)", msg.Payload.size());
					break;
				}

				ConnectionInfo* ci    = ConnectionMgr->FindConnection(msg.Connection);
				const uint8_t ownerID = ci ? ci->OwnerID : 0;
				World* clientWorld    = WorldMap[ownerID];
				if (!clientWorld)
				{
					LOG_ENG_WARN_F("[ClientNet] ConstructSpawn but no client world for OwnerID %u", ownerID);
					break;
				}

				DeferredConstructSpawn deferred{ownerID, msg.Header.FrameNumber, msg.Payload};

				// Try immediately — if entities aren't ready yet, push to deferred queue.
				if (!TrySpawnDeferred(deferred)) DeferredConstructSpawns.push_back(std::move(deferred));
				break;
			}

		case NetMessageType::StateCorrection:
			{
				if (msg.Payload.size() < sizeof(StateCorrectionEntry))
				{
					LOG_ENG_WARN("[ClientNet] StateCorrection payload too small");
					break;
				}

				ConnectionInfo* ci    = ConnectionMgr->FindConnection(msg.Connection);
				const uint8_t ownerID = ci ? ci->OwnerID : 0;
				World* clientWorld    = WorldMap[ownerID];
				if (!clientWorld || !ci) break;

				const uint32_t clientFrame = msg.Header.FrameNumber;

				const uint32_t entryCount = static_cast<uint32_t>(
					msg.Payload.size() / sizeof(StateCorrectionEntry));
				const auto* entries = reinterpret_cast<const StateCorrectionEntry*>(msg.Payload.data());

				Registry* corrReg      = clientWorld->GetRegistry();
				LogicThread* corrLogic = clientWorld->GetLogicThread();

				// Heap-allocate so the fire-and-forget Post lambda can safely outlive this scope.
				// The lambda owns the vector and deletes it after use.
				struct CorrCapture
				{
					Registry* reg;
					LogicThread* logic;
					std::vector<StateCorrectionEntry>* corrs;
					uint32_t clientFrame;
					uint32_t LastAckedFrame;
				};
				static_assert(sizeof(CorrCapture) <= 48, "CorrCapture exceeds job payload limit");

				auto* corrHeap = new std::vector<StateCorrectionEntry>(entries, entries + entryCount);
				CorrCapture cap{corrReg, corrLogic, corrHeap, clientFrame, ci->LastServerAckedFrame};
				clientWorld->Post([cap](uint32_t)
				{
					ReplicationSystem::HandleStateCorrections(cap.reg, cap.corrs->data(),
															  static_cast<uint32_t>(cap.corrs->size()), cap.clientFrame, cap.logic, cap.LastAckedFrame);
					delete cap.corrs;
				});
				break;
			}

		case NetMessageType::EntityDestroy:
			// TODO
			break;

		case NetMessageType::SoulRPC:
			{
				// Inbound server→client SoulRPC. Route to client-side Soul via FlowManager.
				ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
				if (!ci) break;

				if (msg.Payload.size() >= sizeof(RPCHeader))
				{
					const auto* rpcHdrPeek = reinterpret_cast<const RPCHeader*>(msg.Payload.data());
					LOG_ENG_INFO_F("[ClientNet] SoulRPC received: ownerID=%u methodID=%u repState=%d",
								   ci->OwnerID, rpcHdrPeek->MethodID, static_cast<int>(ci->RepState));
				}

				if (msg.Payload.size() < sizeof(RPCHeader))
				{
					LOG_ENG_WARN_F("[ClientNet] SoulRPC payload too small (%zu bytes)", msg.Payload.size());
					break;
				}

				const auto* rpcHdr      = reinterpret_cast<const RPCHeader*>(msg.Payload.data());
				const uint8_t* params   = msg.Payload.data() + sizeof(RPCHeader);
				const size_t paramBytes = msg.Payload.size() - sizeof(RPCHeader);

				if (paramBytes < rpcHdr->ParamSize)
				{
					LOG_ENG_WARN_F("[ClientNet] SoulRPC param underrun (MethodID=%u, want=%u, got=%zu)",
								   rpcHdr->MethodID, rpcHdr->ParamSize, paramBytes);
					break;
				}

				{
					World* clientWorld = WorldMap[ci->OwnerID];
					if (FlowManager* flow = clientWorld ? clientWorld->GetFlowManager() : nullptr)
					{
						if (Soul* soul = flow->GetSoul(ci->OwnerID))
						{
							RPCContext ctx{ci, ConnectionMgr};
							soul->DispatchClientRPC(ctx, *rpcHdr, params);
						}
						else
						{
							LOG_NET_WARN_F(soul, "[ClientNet] SoulRPC: no Soul for ownerID=%u (MethodID=%u)",
										   ci->OwnerID, rpcHdr->MethodID);
						}
					}
				}
				break;
			}

		// Legacy cases — superseded by SoulRPC. Kept for wire-compat; remove once server is updated.
		case NetMessageType::PlayerBeginConfirm:
			LOG_ENG_WARN("[ClientNet] Received legacy PlayerBeginConfirm — server should use SoulRPC");
			break;

		case NetMessageType::PlayerBeginReject:
			LOG_ENG_WARN("[ClientNet] Received legacy PlayerBeginReject — server should use SoulRPC");
			break;

		case NetMessageType::GameModeManifest:
			{
				// Engine validates that at least the base header fields arrived.
				// The GameMode is responsible for casting to its own concrete derived type.
				struct BaseManifest : GameModeManifestPayload<BaseManifest>
				{
				};
				if (msg.Header.PayloadSize < sizeof(BaseManifest))
				{
					LOG_ENG_WARN_F("[ClientNet] GameModeManifest too small (got %u)", msg.Header.PayloadSize);
					break;
				}
				// TODO: forward raw bytes to GameMode::OnGameModeManifest(payload, size)
				const auto* base = reinterpret_cast<const BaseManifest*>(msg.Payload.data());
				LOG_ENG_INFO_F("[ClientNet] GameModeManifest received (seq=%u, mode='%s') — GameMode routing not yet wired",
							   base->SequenceID, base->ModeName);
				break;
			}

		default:
			LOG_ENG_WARN_F("[ClientNet] Unhandled message type %u", msg.Header.Type);
			break;
	}
}

// ---------------------------------------------------------------------------

bool ClientNetThread::TrySpawnDeferred(const DeferredConstructSpawn& entry)
{
	World* clientWorld = WorldMap[entry.OwnerID];
	if (!clientWorld) return true; // World gone — drop it

	ConstructRegistry* constructs = clientWorld->GetConstructRegistry();
	Registry* entityReg           = clientWorld->GetRegistry();
	std::vector<uint8_t> payload  = entry.Payload;

	bool done                  = false;
	const uint8_t* payloadData = payload.data();
	const size_t payloadSize   = payload.size();
	bool* pDone                = &done;
	clientWorld->SpawnAndWait([constructs, entityReg, clientWorld, payloadData, payloadSize, pDone](uint32_t)
	{
		*pDone = ReplicationSystem::HandleConstructSpawn(
			constructs, entityReg, clientWorld, payloadData, payloadSize);
	});

	// If the construct was successfully created, request a rollback to its server spawn frame
	// so any backing entity temporal data is populated from the correct historical frame.
#ifdef TNX_ENABLE_ROLLBACK
	if (done && entry.ServerSpawnFrame > 0 && payload.size() >= sizeof(ConstructSpawnPayload))
	{
		LogicThread* logic = clientWorld->GetLogicThread();
		if (logic)
		{
			logic->EnqueueSpawnRollback(entry.ServerSpawnFrame);
		}
	}
#endif

	return done;
}

void ClientNetThread::FlushDeferredEntitySpawns()
{
	for (auto it = DeferredEntitySpawns.begin(); it != DeferredEntitySpawns.end();)
	{
		World* clientWorld = WorldMap[it->OwnerID];
		if (!clientWorld)
		{
			it = DeferredEntitySpawns.erase(it);
			continue;
		}

		if (it->Payload.size() < sizeof(EntitySpawnPayload))
		{
			LOG_ENG_WARN_F("[ClientNet] Deferred EntitySpawn payload too small (%zu)", it->Payload.size());
			it = DeferredEntitySpawns.erase(it);
			continue;
		}

		EntitySpawnPayload spawnData = *reinterpret_cast<const EntitySpawnPayload*>(it->Payload.data());
		Registry* spawnReg           = clientWorld->GetRegistry();
		uint32_t& SpawnFrame         = it->ServerSpawnFrame;
		clientWorld->SpawnAndWait([spawnReg, &spawnData, SpawnFrame](uint32_t)
		{
			ReplicationSystem::HandleEntitySpawn(spawnReg, spawnData, SpawnFrame);
		});

		// Request a rollback to the entity's server spawn frame so the entity is inserted
		// at the correct historical ring slot. ReplayServerEventsAt will re-hydrate it.
#ifdef TNX_ENABLE_ROLLBACK
		LogicThread* logic = clientWorld->GetLogicThread();
		if (logic && it->ServerSpawnFrame > 0)
		{
			logic->EnqueueSpawnRollback(it->ServerSpawnFrame);
		}
#endif

		it = DeferredEntitySpawns.erase(it);
	}
}

void ClientNetThread::TickReplication()
{
	// Retry PlayerBeginRequest for connections stuck in Loaded state.
	constexpr uint64_t RetryIntervalMs = 150;
	const uint64_t now                 = SDL_GetTicks();

	for (auto& ci : ConnectionMgr->GetConnections())
	{
		if (!ci.bClientInitiated || !ci.bConnected) continue;
		if (ci.RepState != ClientRepState::Loaded) continue;
		if (ci.PlayerBeginSentAt == 0) continue;
		if (now - ci.PlayerBeginSentAt < RetryIntervalMs) continue;

		World* world      = WorldMap[ci.OwnerID];
		FlowManager* flow = world ? world->GetFlowManager() : nullptr;
		if (!flow) continue;

		LOG_ENG_WARN_F("[ClientNet] PlayerBeginRequest retry (ownerID=%u, %.0fms since last send)",
					   ci.OwnerID, static_cast<float>(now - ci.PlayerBeginSentAt));

		ConnectionInfo* mci = ConnectionMgr->FindConnection(ci.Handle);
		if (!mci) continue;

		flow->SendPlayerBeginRequest(NetChannel(mci, ConnectionMgr), 0, mci->Predictions);
		mci->PlayerBeginSentAt = now;
	}

	// EntitySpawns must land before ConstructSpawns reference them.
	FlushDeferredEntitySpawns();

	if (DeferredConstructSpawns.empty()) return;

	auto it = DeferredConstructSpawns.begin();
	while (it != DeferredConstructSpawns.end())
	{
		if (TrySpawnDeferred(*it)) it = DeferredConstructSpawns.erase(it);
		else ++it;
	}
}

void ClientNetThread::TickInputSend()
{
	if (!TrinyxJobs::IsRunning()) return;

	// Skip if the previous job is still running — single-consumer MPSC ring
	// can't safely be accessed by two jobs concurrently.
	if (SendCounter.Value.load(std::memory_order_acquire) != 0) return;

	ClientNetThread* self = this;
	TrinyxJobs::Dispatch([self](uint32_t) { self->ExecuteInputSend(); },
						 &SendCounter, TrinyxJobs::Queue::General);
}

void ClientNetThread::ExecuteInputSend()
{
	std::vector<HSteamNetConnection> clientHandles;
	for (const auto& ci : ConnectionMgr->GetConnections())
		if (ci.bClientInitiated && ci.bConnected && ci.OwnerID != 0)
			clientHandles.push_back(ci.Handle);

	for (HSteamNetConnection handle : clientHandles)
	{
		ConnectionInfo* ci = ConnectionMgr->FindConnection(handle);
		if (!ci) continue;

		if (ci->RepState < ClientRepState::Playing) continue;

		World* world = WorldMap[ci->OwnerID];
		if (!world) continue;

		auto* consumer = world->GetInputAccumConsumer();
		if (!consumer) continue;

		// Drop floor — entirely in client-local frame space.
		// Pre-handshake junk: frames before ClientLocalFrameAtHandshake are pre-connection noise.
		// Already-acked: LastServerAckedFrame is echoed back by the server in client-local
		// space (server converts from server-frame via log->FrameOffset before stamping).
		const uint32_t preHandshakeFloor = ci->ClientLocalFrameAtHandshake > 0
											   ? ci->ClientLocalFrameAtHandshake - 1
											   : 0u;

		// Window backstop: if acks stall (e.g. server is stalled and frozen its ack), the
		// accumulator can grow well past MaxWindowFrames. The client would then keep sending
		// the oldest MaxWindowFrames entries — frames the server already has — which can
		// never advance LastReceivedFrame and permanently deadlocks the stall.
		// Anchor the drop floor so the accumulator never exceeds MaxWindowFrames entries.
		uint32_t windowBackstop = 0;
		if (consumer->Size() > static_cast<size_t>(MaxWindowFrames))
		{
			NetInputFrame tail;
			if (consumer->TryPeekAt(consumer->Size() - 1, tail) && tail.Frame >= MaxWindowFrames) windowBackstop = tail.Frame - MaxWindowFrames;
		}

		const uint32_t dropFloor = std::max({ci->LastServerAckedFrame, preHandshakeFloor, windowBackstop});

		size_t dropCount = 0;
		while (dropCount < consumer->Size())
		{
			NetInputFrame front;
			if (!consumer->TryPeekAt(dropCount, front)) break;
			if (front.Frame > dropFloor) break;
			++dropCount;
		}
		if (dropCount > 0) consumer->DropFront(dropCount);

		// Peek up to MaxWindowFrames unacked entries.  Frame numbers are client-local —
		// the server translates to server-frame space via PlayerInputLog::FrameOffset.
		InputWindowPacket wirePayload{};
		const uint32_t count = static_cast<uint32_t>(
			std::min(consumer->Size(), static_cast<size_t>(MaxWindowFrames)));

		for (uint32_t i = 0; i < count; ++i)
		{
			NetInputFrame frame;
			if (!consumer->TryPeekAt(i, frame)) break;
			wirePayload.Frames[i]  = frame;
			wirePayload.FrameCount = i + 1;
		}

		if (wirePayload.FrameCount == 0) continue;

		wirePayload.FirstFrame         = wirePayload.Frames[0].Frame;
		const uint32_t lastClientFrame = wirePayload.Frames[wirePayload.FrameCount - 1].Frame;

		//LOG_ENG_INFO_F("[ClientNet] Sending %u input frames (first=%u, last=%u)", wirePayload.FrameCount, wirePayload.FirstFrame, lastClientFrame);

		PacketHeader header{};
		header.Type        = static_cast<uint8_t>(NetMessageType::InputFrame);
		header.Flags       = PacketFlag::DefaultFlags;
		header.PayloadSize = sizeof(InputWindowPacket);
		header.FrameNumber = lastClientFrame; // client-local; server translates via FrameOffset
		header.SenderID    = ci->OwnerID;
		ConnectionMgr->Send(handle, header,
							reinterpret_cast<const uint8_t*>(&wirePayload), false, /*noNagle=*/true);
	}
}
