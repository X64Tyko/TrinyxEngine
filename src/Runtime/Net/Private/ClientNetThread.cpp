#include "ClientNetThread.h"

#include "CacheSlotMeta.h"
#include "FlowManager.h"
#include "NetChannel.h"
#include "NetConnectionManager.h"
#include "NetTypes.h"
#include "Registry.h"
#include "ReplicationSystem.h"
#include "TemporalComponentCache.h"
#include "World.h"
#include "Logger.h"

#include <SDL3/SDL_timer.h>

void ClientNetThread::HandleMessage(const ReceivedMessage& msg)
{
	auto type = static_cast<NetMessageType>(msg.Header.Type);

	switch (type)
	{
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
				LOG_WARN_F("[ClientNet] ConnectionHandshake from unknown connection %u", msg.Connection);
				break;
			}

			if (msg.Header.SenderID == 0)
			{
				LOG_WARN("[ClientNet] Invalid HandshakeAccept — SenderID is 0");
				break;
			}

			ConnectionMgr->AssignOwnerID(msg.Connection, msg.Header.SenderID);

			if (msg.Payload.size() >= sizeof(HandshakePayload))
			{
				const auto* hsPay      = reinterpret_cast<const HandshakePayload*>(msg.Payload.data());
				ci->ServerFrameAtHandshake = hsPay->ServerFrame;
			}
			ci->RepState = ClientRepState::Synchronizing;

			LOG_INFO_F("[ClientNet] HandshakeAccept received — OwnerID=%u serverFrame=%u",
					   msg.Header.SenderID, ci->ServerFrameAtHandshake);
			break;
		}

		case NetMessageType::ClockSync:
		{
			ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
			if (!ci) break;

			if (msg.Payload.size() < sizeof(ClockSyncPayload))
			{
				LOG_WARN_F("[ClientNet] ClockSync payload too small (%zu)", msg.Payload.size());
				break;
			}

			const uint32_t tickRate = (Config->FixedUpdateHz == EngineConfig::Unset)
										  ? 128u
										  : static_cast<uint32_t>(Config->FixedUpdateHz);
			const float stepMs = 1000.0f / static_cast<float>(tickRate);
			ci->InputLead      = static_cast<uint32_t>(ci->RTT_ms * 0.5f / stepMs) + 2u;
			ci->RepState       = ClientRepState::Loading;

			LOG_INFO_F("[ClientNet] ClockSync complete — InputLead=%u RTT=%.1fms → Loading",
					   ci->InputLead, ci->RTT_ms);
			break;
		}

		case NetMessageType::TravelNotify:
		{
			ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
			if (!ci) break;

			if (msg.Payload.size() < sizeof(TravelPayload))
			{
				LOG_WARN_F("[ClientNet] TravelNotify payload too small (%zu)", msg.Payload.size());
				break;
			}

			const auto* travelMsg = reinterpret_cast<const TravelPayload*>(msg.Payload.data());
			ci->RepState          = ClientRepState::LevelLoading;

			LOG_INFO_F("[ClientNet] TravelNotify received — loading level '%s'", travelMsg->LevelPath);

			if (FlowMgr) FlowMgr->PostTravelNotify(travelMsg->LevelPath);

			// Auto-acknowledge (synchronous load in PIE).
			// Future: remove and let FlowState call AcknowledgeLevelReady() after async load.
			NetChannel(ci, ConnectionMgr).SendHeaderOnly(NetMessageType::LevelReady, /*reliable=*/true);

			ci->RepState = ClientRepState::LevelLoaded;
			LOG_INFO("[ClientNet] LevelReady sent → client LevelLoaded");
			break;
		}

		case NetMessageType::FlowEvent:
		{
			if (msg.Payload.size() < sizeof(FlowEventPayload))
			{
				LOG_WARN_F("[ClientNet] FlowEvent payload too small (%zu)", msg.Payload.size());
				break;
			}

			const auto* ev     = reinterpret_cast<const FlowEventPayload*>(msg.Payload.data());
			ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
			if (!ci) break;

			if (ci->RepState == ClientRepState::LevelLoaded
				&& ev->EventID == static_cast<uint8_t>(FlowEventID::ServerReady))
			{
				ci->RepState = ClientRepState::Loaded;

				World* clientWorld = WorldMap[ci->OwnerID];
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
						int sweepCount = 0;
						for (uint32_t i = 0; i < max; ++i)
						{
							const uint32_t f    = static_cast<uint32_t>(flags[i]);
							const uint32_t mask = -((f & aliveBit) >> aliveShift);
							sweepCount         += static_cast<int>((activeBit & mask & ~f) >> activeShift);
							flags[i]            = static_cast<int32_t>(f | (activeBit & mask));
						}
						LOG_INFO_F("[Replication] ServerReady: swept %d Alive→Active", sweepCount);
					});
				}
				LOG_INFO("[ClientNet] FlowEvent::ServerReady → Loaded, Alive→Active sweep enqueued");
			}

			if (FlowMgr) FlowMgr->PostNetEvent(ev->EventID);
			break;
		}

		case NetMessageType::EntitySpawn:
		{
			if (msg.Payload.size() < sizeof(EntitySpawnPayload))
			{
				LOG_WARN_F("[ClientNet] EntitySpawn payload too small (%zu)", msg.Payload.size());
				break;
			}

			const auto* spawn  = reinterpret_cast<const EntitySpawnPayload*>(msg.Payload.data());
			ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
			const uint8_t ownerID = ci ? ci->OwnerID : 0;
			World* clientWorld    = WorldMap[ownerID];
			if (!clientWorld)
			{
				LOG_WARN_F("[ClientNet] EntitySpawn but no client world for OwnerID %u", ownerID);
				break;
			}

			const EntitySpawnPayload spawnData = *spawn;
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
				LOG_WARN("[ClientNet] StateCorrection payload too small");
				break;
			}

			ConnectionInfo* ci    = ConnectionMgr->FindConnection(msg.Connection);
			const uint8_t ownerID = ci ? ci->OwnerID : 0;
			World* clientWorld    = WorldMap[ownerID];
			if (!clientWorld) break;

			const uint32_t entryCount = static_cast<uint32_t>(
				msg.Payload.size() / sizeof(StateCorrectionEntry));
			const auto* entries = reinterpret_cast<const StateCorrectionEntry*>(msg.Payload.data());
			std::vector<StateCorrectionEntry> corrections(entries, entries + entryCount);

			clientWorld->Spawn([corrections](Registry* reg)
			{
				ReplicationSystem::HandleStateCorrections(
					reg, corrections.data(), static_cast<uint32_t>(corrections.size()));
			});
			break;
		}

		case NetMessageType::EntityDestroy:
			// TODO
			break;

		default:
			LOG_WARN_F("[ClientNet] Unhandled message type %u", msg.Header.Type);
			break;
	}
}
