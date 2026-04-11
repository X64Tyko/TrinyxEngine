#include "NetThread.h"
#include "FlowManager.h"
#include "GNSContext.h"
#include "NetConnectionManager.h"
#include "ReplicationSystem.h"
#include "EngineConfig.h"
#include "Input.h"
#include "Logger.h"
#include "LogicThread.h"
#include "Profiler.h"
#include "ThreadPinning.h"
#include "World.h"

#include "CacheSlotMeta.h"
#include "Registry.h"

#include <SDL3/SDL_timer.h>
#include <cstring>

NetThread::NetThread()  = default;
NetThread::~NetThread() = default;

void NetThread::Initialize(GNSContext* gns, const EngineConfig* config)
{
	GNS    = gns;
	Config = config;

	ConnectionMgr = std::make_unique<NetConnectionManager>();
	ConnectionMgr->Initialize(gns);

	LOG_INFO("[NetThread] Initialized");
}

// ---------------------------------------------------------------------------
// Threaded mode
// ---------------------------------------------------------------------------

void NetThread::Start()
{
	bIsRunning.store(true, std::memory_order_release);
	Thread = std::thread(&NetThread::ThreadMain, this);
	TrinyxThreading::PinThread(Thread);
	LOG_INFO("[NetThread] Started (threaded mode)");
}

void NetThread::Stop()
{
	bIsRunning.store(false, std::memory_order_release);
	LOG_INFO("[NetThread] Stop requested");
}

void NetThread::Join()
{
	if (Thread.joinable())
	{
		Thread.join();
		LOG_INFO("[NetThread] Joined");
	}
}

void NetThread::ThreadMain()
{
	const uint64_t perfFrequency = SDL_GetPerformanceFrequency();
	const double stepTime        = Config->GetNetworkStepTime();

	while (bIsRunning.load(std::memory_order_acquire))
	{
		TNX_ZONE_NC("Net_Frame", 0xFF8844);

		const uint64_t frameStart = SDL_GetPerformanceCounter();

		Tick();

		// Rate limit to NetworkUpdateHz
		const uint64_t targetTicks = static_cast<uint64_t>(stepTime * static_cast<double>(perfFrequency));
		const uint64_t frameEnd    = frameStart + targetTicks;

		uint64_t now = SDL_GetPerformanceCounter();
		if (frameEnd > now)
		{
			const double remainingSec       = static_cast<double>(frameEnd - now) / static_cast<double>(perfFrequency);
			constexpr double SleepMarginSec = 0.001;
			if (remainingSec > SleepMarginSec) SDL_Delay(static_cast<uint32_t>((remainingSec - SleepMarginSec) * 1000.0));

			while (SDL_GetPerformanceCounter() < frameEnd)
			{
				/* busy wait */
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Inline mode / shared work
// ---------------------------------------------------------------------------

void NetThread::Tick()
{
	TNX_ZONE_N("Net_Tick");

	// Run GNS internal callbacks (connection state changes)
	ConnectionMgr->RunCallbacks();

	// Poll all incoming messages
	std::vector<ReceivedMessage> messages;
	ConnectionMgr->PollIncoming(messages);

	// Route each message
	for (const auto& msg : messages)
	{
		RouteMessage(msg);
	}

	// Server-side replication: send entity state to connected clients.
	// Frame number comes from the server's LogicThread — not a local counter.
	if (Replicator)
	{
		Replicator->Tick(ConnectionMgr.get());
	}

	// Clock sync probes and heartbeat.
	// Runs for all bClientInitiated connections — these are legs we opened via Connect().
	// In GNS loopback (same-process PIE), bServerSide is unreliable because both ends
	// of a loopback pair may have m_hListenSocket==0. bClientInitiated is set explicitly
	// in Connect() and is always correct.
	{
		const double nowSec = static_cast<double>(SDL_GetPerformanceCounter())
			/ static_cast<double>(SDL_GetPerformanceFrequency());
		const uint8_t probeTarget = static_cast<uint8_t>(
			(Config->ClockSyncProbes == EngineConfig::Unset) ? 8 : Config->ClockSyncProbes);

		// Snapshot handles to avoid iterator invalidation while mutating ConnectionInfo.
		std::vector<HSteamNetConnection> handles;
		for (const auto& ci : ConnectionMgr->GetConnections()) if (ci.bConnected && ci.bClientInitiated) handles.push_back(ci.Handle);


		for (HSteamNetConnection handle : handles)
		{
			ConnectionInfo* ci = ConnectionMgr->FindConnection(handle);
			if (!ci) continue;

			// During Synchronizing: send probes to build RTT, then ClockSyncRequest.
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
					// RTT estimate is stable — send ClockSyncRequest.
					ClockSyncPayload csReq{};
					csReq.ClientTimestamp = SDL_GetPerformanceCounter();
					csReq.ServerFrame     = 0;

					PacketHeader header{};
					header.Type        = static_cast<uint8_t>(NetMessageType::ClockSync);
					header.Flags       = PacketFlag::DefaultFlags;
					header.SequenceNum = ci->NextSeqOut++;
					header.Timestamp   = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
					header.PayloadSize = sizeof(ClockSyncPayload);
					ConnectionMgr->Send(handle, header,
										reinterpret_cast<const uint8_t*>(&csReq), false);
					ci->ClockSyncProbesSent = 255; // sentinel: past probe phase
					ci->LastHeartbeatTime   = nowSec;
				}
			}

			// Periodic heartbeat Ping (1/sec) past the probe phase.
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

	// Client-side input: for each outbound connection, snapshot its mapped world's
	// NetInput and send an InputFrame to the server. Runs once per net tick per
	// client leg — scales to multi-client PIE without any dedicated ClientWorld pointer.
	{
		std::vector<HSteamNetConnection> clientHandles;
		for (const auto& ci : ConnectionMgr->GetConnections()) if (ci.bClientInitiated && ci.bConnected && ci.OwnerID != 0) clientHandles.push_back(ci.Handle);

		for (HSteamNetConnection handle : clientHandles)
		{
			ConnectionInfo* ci = ConnectionMgr->FindConnection(handle);
			if (!ci) continue;

			World* world = WorldMap[ci->OwnerID];
			if (!world) continue;

			InputBuffer* netInput = world->GetNetInput();
			netInput->Swap(); // consume everything since the last net tick

			const uint32_t frame = world->GetLogicThread()
									   ? world->GetLogicThread()->GetLastCompletedFrame()
									   : 0;

			InputFramePayload payload{};
			netInput->SnapshotKeyState(payload.KeyState, sizeof(payload.KeyState));
			payload.MouseDX      = netInput->GetMouseDX();
			payload.MouseDY      = netInput->GetMouseDY();
			payload.MouseButtons = netInput->GetMouseButtonMask();

			PacketHeader header{};
			header.Type        = static_cast<uint8_t>(NetMessageType::InputFrame);
			header.Flags       = PacketFlag::DefaultFlags;
			header.PayloadSize = sizeof(InputFramePayload);
			header.FrameNumber = frame;
			header.SenderID    = ci->OwnerID;
			ConnectionMgr->Send(handle, header,
								reinterpret_cast<const uint8_t*>(&payload), false);
		}
	}

	// FPS tracking
	FpsFrameCount++;
	const double now = SDL_GetPerformanceCounter() / static_cast<double>(SDL_GetPerformanceFrequency());
	FpsTimer         += now - LastFPSCheck;
	LastFPSCheck     = now;

	if (FpsTimer >= 1.0) [[unlikely]]
	{
		float fps = static_cast<float>(FpsFrameCount / FpsTimer);
		float ms  = static_cast<float>((FpsTimer / FpsFrameCount) * 1000.0);
		NetFPS.store(fps, std::memory_order_relaxed);
		NetFrameMs.store(ms, std::memory_order_relaxed);
		LOG_DEBUG_F("Net FPS: %d | Frame: %.2fms", static_cast<int>(fps), static_cast<double>(ms));
		FpsFrameCount = 0;
		FpsTimer      = 0.0;
	}
}

// ---------------------------------------------------------------------------
// Message routing
// ---------------------------------------------------------------------------

void NetThread::MapConnectionToWorld(uint8_t ownerID, World* world)
{
	WorldMap[ownerID] = world;
	LOG_INFO_F("[NetThread] Mapped OwnerID %u to World %p", ownerID, static_cast<void*>(world));
}

void NetThread::MapConnectionToFlow(uint8_t ownerID, FlowManager* flow)
{
	FlowMap[ownerID] = flow;
	LOG_INFO_F("[NetThread] Mapped OwnerID %u to FlowManager %p", ownerID, static_cast<void*>(flow));
}

void NetThread::RouteMessage(const ReceivedMessage& msg)
{
	auto type = static_cast<NetMessageType>(msg.Header.Type);

	switch (type)
	{
		case NetMessageType::InputFrame:
			{
				uint8_t ownerID = msg.Header.SenderID;
				World* world    = WorldMap[ownerID];
				if (!world)
				{
					LOG_WARN_F("[NetThread] InputFrame from unmapped OwnerID %u — dropped", ownerID);
					break;
				}

				if (msg.Payload.size() < sizeof(InputFramePayload))
				{
					LOG_WARN_F("[NetThread] InputFrame payload too small (%zu < %zu)",
							   msg.Payload.size(), sizeof(InputFramePayload));
					break;
				}

				const auto* payload = reinterpret_cast<const InputFramePayload*>(msg.Payload.data());

				// Write directly into the World's SimInput — same buffer LogicThread reads.
				InputBuffer* simInput = world->GetSimInput();
				simInput->InjectState(payload->KeyState, payload->MouseDX, payload->MouseDY, payload->MouseButtons);
				break;
			}

		case NetMessageType::Ping:
			{
				// Respond with Pong on the same connection
				PacketHeader pong{};
				pong.Type        = static_cast<uint8_t>(NetMessageType::Pong);
				pong.Flags       = PacketFlag::DefaultFlags;
				pong.SequenceNum = msg.Header.SequenceNum;
				pong.FrameNumber = msg.Header.FrameNumber;
				pong.SenderID    = 0;
				pong.Timestamp   = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
				ConnectionMgr->Send(msg.Connection, pong, nullptr, false);
				break;
			}

		case NetMessageType::Pong:
			{
				ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
				if (ci)
				{
					uint16_t now  = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
					uint16_t sent = msg.Header.Timestamp;
					float rtt     = static_cast<float>(static_cast<uint16_t>(now - sent));
					if (ci->RTT_ms <= 0.0f) ci->RTT_ms = rtt;
					else ci->RTT_ms                    = ci->RTT_ms * 0.875f + rtt * 0.125f;

					// Count probes received during the Synchronizing clock-sync phase.
					if (ci->bClientInitiated
						&& ci->RepState == ClientRepState::Synchronizing
						&& ci->ClockSyncProbesRecvd < 8)
					{
						ci->ClockSyncProbesRecvd++;
					}
				}
				break;
			}

		case NetMessageType::EntitySpawn:
			{
				if (msg.Payload.size() < sizeof(EntitySpawnPayload))
				{
					LOG_WARN_F("[NetThread] EntitySpawn payload too small (%zu < %zu)",
							   msg.Payload.size(), sizeof(EntitySpawnPayload));
					break;
				}

				const auto* spawn = reinterpret_cast<const EntitySpawnPayload*>(msg.Payload.data());

				// Find which client world this connection maps to
				ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
				uint8_t ownerID    = ci ? ci->OwnerID : 0;
				World* clientWorld = WorldMap[ownerID];
				if (!clientWorld)
				{
					LOG_WARN_F("[NetThread] EntitySpawn but no client world for OwnerID %u", ownerID);
					break;
				}

				// Capture payload for the spawn lambda
				EntitySpawnPayload spawnData = *spawn;

				clientWorld->Spawn([spawnData](Registry* reg)
				{
					ReplicationSystem::HandleEntitySpawn(reg, spawnData);
				});
				break;
			}

		case NetMessageType::StateCorrection:
			{
				if (msg.Payload.size() < sizeof(StateCorrectionEntry))
				{
					LOG_WARN("[NetThread] StateCorrection payload too small");
					break;
				}

				ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
				uint8_t ownerID    = ci ? ci->OwnerID : 0;
				World* clientWorld = WorldMap[ownerID];
				if (!clientWorld) break;

				const uint32_t entryCount = static_cast<uint32_t>(
					msg.Payload.size() / sizeof(StateCorrectionEntry));
				const auto* entries = reinterpret_cast<const StateCorrectionEntry*>(
					msg.Payload.data());

				// Batch all corrections into a single Spawn call for thread safety
				std::vector<StateCorrectionEntry> corrections(entries, entries + entryCount);

				clientWorld->Spawn([corrections](Registry* reg)
				{
					ReplicationSystem::HandleStateCorrections(
						reg, corrections.data(), static_cast<uint32_t>(corrections.size()));
				});
				break;
			}

		case NetMessageType::ConnectionHandshake:
			{
				auto connection = msg.Connection;
				auto* ci        = ConnectionMgr->FindConnection(connection);
				if (!ci)
				{
					LOG_WARN_F("[NetThread] ConnectionHandshake from unknown connection %u", connection);
					break;
				}

				if (ci->bServerSide)
				{
					// Server accepts client join: assign OwnerID, stamp server frame, send HandshakePayload.
					if (msg.Header.SenderID != 0)
					{
						LOG_WARN_F("[NetThread] ConnectionHandshake from client with existing ownerID %u", msg.Header.SenderID);
						break;
					}

					ConnectionMgr->GenerateNetID(connection);

					// Stamp server frame for clock sync anchor
					const uint32_t serverFrame = (WorldMap[0] && WorldMap[0]->GetLogicThread())
													 ? WorldMap[0]->GetLogicThread()->GetLastCompletedFrame()
													 : 0;
					ci->ServerFrameAtHandshake = serverFrame;
					ci->RepState               = ClientRepState::Synchronizing;

					HandshakePayload hsPay{};
					hsPay.TickRate = static_cast<uint32_t>(
						Config->FixedUpdateHz == EngineConfig::Unset ? 128 : Config->FixedUpdateHz);
					hsPay.ServerFrame = serverFrame;

					PacketHeader handshakeHeader{};
					handshakeHeader.Type        = static_cast<uint8_t>(NetMessageType::ConnectionHandshake);
					handshakeHeader.Flags       = PacketFlag::DefaultFlags;
					handshakeHeader.SequenceNum = 2;
					handshakeHeader.FrameNumber = serverFrame;
					handshakeHeader.SenderID    = ci->OwnerID;
					handshakeHeader.Timestamp   = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
					handshakeHeader.PayloadSize = sizeof(HandshakePayload);
					ConnectionMgr->Send(connection, handshakeHeader,
										reinterpret_cast<const uint8_t*>(&hsPay), true);

					LOG_INFO_F("[NetThread] HandshakeAccept → OwnerID=%u frame=%u tickRate=%u",
							   ci->OwnerID, serverFrame, hsPay.TickRate);
				}
				else
				{
					// Client receives server accept: store bootstrap data, advance RepState.
					if (msg.Header.SenderID == 0)
					{
						LOG_WARN("[NetThread] Invalid HandshakeAccept — SenderID is 0");
						break;
					}

					ConnectionMgr->AssignOwnerID(connection, msg.Header.SenderID);

					if (msg.Payload.size() >= sizeof(HandshakePayload))
					{
						const auto* hsPay          = reinterpret_cast<const HandshakePayload*>(msg.Payload.data());
						ci->ServerFrameAtHandshake = hsPay->ServerFrame;
					}
					ci->RepState = ClientRepState::Synchronizing;

					LOG_INFO_F("[NetThread] HandshakeAccept received — OwnerID=%u serverFrame=%u",
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
					LOG_WARN_F("[NetThread] ClockSync payload too small (%zu)", msg.Payload.size());
					break;
				}

				const auto* req = reinterpret_cast<const ClockSyncPayload*>(msg.Payload.data());

				if (ci->bServerSide)
				{
					// Server responds: echo ClientTimestamp, fill ServerFrame.
					const uint32_t serverFrame = (WorldMap[0] && WorldMap[0]->GetLogicThread())
													 ? WorldMap[0]->GetLogicThread()->GetLastCompletedFrame()
													 : 0;
					ClockSyncPayload resp{};
					resp.ClientTimestamp = req->ClientTimestamp;
					resp.ServerFrame     = serverFrame;

					PacketHeader header{};
					header.Type        = static_cast<uint8_t>(NetMessageType::ClockSync);
					header.Flags       = PacketFlag::DefaultFlags;
					header.SequenceNum = ci->NextSeqOut++;
					header.Timestamp   = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
					header.SenderID    = 0;
					header.PayloadSize = sizeof(ClockSyncPayload);
					ConnectionMgr->Send(msg.Connection, header,
										reinterpret_cast<const uint8_t*>(&resp), false);

					// Immediately send TravelNotify — level is loaded on the server before
					// clients connect in PIE. Future: gate on server level-ready state.
					const std::string localPath = FlowMgr ? FlowMgr->GetActiveLevelLocalPath() : std::string{};
					if (!localPath.empty())
					{
						TravelPayload travelMsg{};
						travelMsg.PathLength = static_cast<uint8_t>(
							std::min(localPath.size(), size_t(254)));
						if (localPath.size() > 254)
							LOG_WARN_F("[NetThread] Level path truncated to 254 chars: %s", localPath.c_str());
						std::memcpy(travelMsg.LevelPath, localPath.c_str(), travelMsg.PathLength);
						travelMsg.LevelPath[travelMsg.PathLength] = '\0';

						PacketHeader travelHeader{};
						travelHeader.Type        = static_cast<uint8_t>(NetMessageType::TravelNotify);
						travelHeader.Flags       = PacketFlag::DefaultFlags;
						travelHeader.SequenceNum = ci->NextSeqOut++;
						travelHeader.SenderID    = 0;
						travelHeader.FrameNumber = serverFrame;
						travelHeader.PayloadSize = sizeof(TravelPayload);
						ConnectionMgr->Send(msg.Connection, travelHeader,
											reinterpret_cast<const uint8_t*>(&travelMsg), true);

						ci->RepState = ClientRepState::LevelLoading;
						LOG_INFO_F("[NetThread] ClockSyncResponse + TravelNotify → client LevelLoading (frame=%u, level=%s)",
								   serverFrame, travelMsg.LevelPath);
					}
					else
					{
						ci->RepState = ClientRepState::Loading;
						LOG_INFO_F("[NetThread] ClockSyncResponse sent → client Loading (frame=%u) [no level loaded]",
								   serverFrame);
					}
				}
				else
				{
					// Client: compute InputLead from RTT and advance to Loading.
					const uint32_t tickRate = (Config->FixedUpdateHz == EngineConfig::Unset)
												  ? 128u
												  : static_cast<uint32_t>(Config->FixedUpdateHz);
					const float stepMs = 1000.0f / static_cast<float>(tickRate);
					// InputLead = ceil(RTT/2 / stepMs) + 2 frames of safety
					const uint32_t lead = static_cast<uint32_t>(ci->RTT_ms * 0.5f / stepMs) + 2u;
					ci->InputLead       = lead;
					ci->RepState        = ClientRepState::Loading;
					LOG_INFO_F("[NetThread] ClockSync complete — InputLead=%u RTT=%.1fms → Loading",
							   lead, ci->RTT_ms);
				}
				break;
			}

		case NetMessageType::TravelNotify:
			{
				// Client receives: server is telling us to load a level.
				ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
				if (!ci || ci->bServerSide) break; // Only clients handle this

				if (msg.Payload.size() < sizeof(TravelPayload))
				{
					LOG_WARN_F("[NetThread] TravelNotify payload too small (%zu)", msg.Payload.size());
					break;
				}

				const auto* travelMsg = reinterpret_cast<const TravelPayload*>(msg.Payload.data());
				ci->RepState          = ClientRepState::LevelLoading;

				LOG_INFO_F("[NetThread] TravelNotify received — loading level '%s'",
						   travelMsg->LevelPath);

				// Post to the client's FlowManager so its FlowState can load the level.
				// FlowMap[OwnerID] routes to the correct per-client flow in PIE.
				FlowManager* clientFlow = FlowMap[ci->OwnerID];
				if (clientFlow) clientFlow->PostTravelNotify(travelMsg->LevelPath);

				// Auto-acknowledge: level loading is synchronous in PIE.
				// Future: remove auto-ack and let FlowState call AcknowledgeLevelReady()
				// after async load completes.
				PacketHeader ackHeader{};
				ackHeader.Type        = static_cast<uint8_t>(NetMessageType::LevelReady);
				ackHeader.Flags       = PacketFlag::DefaultFlags;
				ackHeader.SequenceNum = ci->NextSeqOut++;
				ackHeader.SenderID    = ci->OwnerID;
				ackHeader.PayloadSize = 0;
				ConnectionMgr->Send(msg.Connection, ackHeader, nullptr, true);

				ci->RepState = ClientRepState::LevelLoaded;
				LOG_INFO("[NetThread] LevelReady sent → client LevelLoaded");
				break;
			}

		case NetMessageType::LevelReady:
			{
				// Server receives: client finished loading the level.
				ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
				if (!ci || !ci->bServerSide) break; // Only server handles this

				if (ci->RepState == ClientRepState::LevelLoading)
				{
					ci->RepState = ClientRepState::LevelLoaded;
					LOG_INFO_F("[NetThread] LevelReady received — client LevelLoaded (ownerID=%u)", ci->OwnerID);
				}
				break;
			}

		case NetMessageType::FlowEvent:
			{
				if (msg.Payload.size() < sizeof(FlowEventPayload))
				{
					LOG_WARN_F("[NetThread] FlowEvent payload too small (%zu)", msg.Payload.size());
					break;
				}

				const auto* ev = reinterpret_cast<const FlowEventPayload*>(msg.Payload.data());

				ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
				if (ci && !ci->bServerSide)
				{
					// Advance RepState: LevelLoaded → Loaded on ServerReady.
					if (ci->RepState == ClientRepState::LevelLoaded
						&& ev->EventID == static_cast<uint8_t>(FlowEventID::ServerReady))
					{
						ci->RepState = ClientRepState::Loaded;

						// Post Alive→Active sweep to the client world's Logic thread via Spawn().
						// Entities accumulated as Alive-only during level load go visible atomically.
						uint8_t ownerID    = ci->OwnerID;
						World* clientWorld = WorldMap[ownerID];
						if (clientWorld)
						{
							clientWorld->Spawn([](Registry* reg)
							{
								ComponentCacheBase* cache  = reg->GetTemporalCache();
								const uint32_t frame       = cache->GetActiveWriteFrame();
								TemporalFrameHeader* hdr   = cache->GetFrameHeader(frame);
								const ComponentTypeID slot = CacheSlotMeta<>::StaticTemporalIndex();
								auto* flags                = static_cast<int32_t*>(cache->GetFieldData(hdr, slot, 0));
								if (!flags) return;

								const uint32_t max         = cache->GetMaxCachedEntityCount();
								const uint32_t aliveBit    = static_cast<uint32_t>(TemporalFlagBits::Alive);
								const uint32_t activeBit   = static_cast<uint32_t>(TemporalFlagBits::Active);
								const uint32_t aliveShift  = TNX_CTZ32(aliveBit);
								const uint32_t activeShift = TNX_CTZ32(activeBit);
								int sweepCount             = 0;
								for (uint32_t i = 0; i < max; ++i)
								{
									const uint32_t f    = static_cast<uint32_t>(flags[i]);
									const uint32_t mask = -((f & aliveBit) >> aliveShift);                           // alive? 0xFFFFFFFF : 0
									sweepCount          += static_cast<int>((activeBit & mask & ~f) >> activeShift); // 1 only if newly activated
									flags[i]            = static_cast<int32_t>(f | (activeBit & mask));
								}
								LOG_INFO_F("[Replication] ServerReady: swept %d Alive→Active", sweepCount);
							});
						}
						LOG_INFO("[NetThread] FlowEvent::ServerReady → Loaded, Alive→Active sweep enqueued");
					}
				}

				// Post to the client's FlowManager so Sentinel dispatches OnNetEvent.
				if (ci && !ci->bServerSide)
				{
					FlowManager* clientFlow = FlowMap[ci->OwnerID];
					if (clientFlow) clientFlow->PostNetEvent(ev->EventID);
				}
				break;
			}

		case NetMessageType::EntityDestroy:
			{
				// TODO: Not yet implemented
				break;
			}

		default: LOG_WARN_F("[NetThread] Unknown message type %u", msg.Header.Type);
			break;
	}
}
