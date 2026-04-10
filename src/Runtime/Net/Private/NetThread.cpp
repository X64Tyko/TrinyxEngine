#include "NetThread.h"
#include "GNSContext.h"
#include "NetConnectionManager.h"
#include "ReplicationSystem.h"
#include "EngineConfig.h"
#include "Input.h"
#include "Logger.h"
#include "Profiler.h"
#include "ThreadPinning.h"
#include "World.h"

#include "Registry.h"

#include <SDL3/SDL_timer.h>

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

	// Server-side replication: send entity state to connected clients
	if (Replicator)
	{
		static uint32_t replicationFrame = 0;
		Replicator->Tick(ConnectionMgr.get(), replicationFrame++);
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
					// Wrapping 16-bit subtraction handles rollover correctly
					float rtt = static_cast<float>(static_cast<uint16_t>(now - sent));
					// Exponential moving average (alpha=0.125, same as TCP)
					if (ci->RTT_ms <= 0.0f) ci->RTT_ms = rtt;
					else ci->RTT_ms                    = ci->RTT_ms * 0.875f + rtt * 0.125f;
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
					LOG_WARN_F("[NetThread] ConnectionHandshake from unknown connection %ui",
							   connection);
					break;
				}

				if (ci->bServerSide)
				{
					if (msg.Header.SenderID != 0)
					{
						LOG_WARN_F("[NetThread] ConnectionHandshake from client with existing ownerID %ui", msg.Header.SenderID);
						break;
					}

					ConnectionMgr->GenerateNetID(connection);
					PacketHeader handshakeHeader{};
					handshakeHeader.Type        = static_cast<uint8_t>(NetMessageType::ConnectionHandshake);
					handshakeHeader.Flags       = PacketFlag::HasAck;
					handshakeHeader.SequenceNum = 2;
					handshakeHeader.FrameNumber = 0;
					handshakeHeader.SenderID    = ci->OwnerID;
					handshakeHeader.Timestamp   = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
					handshakeHeader.PayloadSize = 0;
					ConnectionMgr->Send(connection, handshakeHeader, nullptr, true);
				}
				else
				{
					if (msg.Header.SenderID == 0)
					{
						LOG_WARN_F("[NetThread] Invalid connection handshake from server with existing ownerID %ui", msg.Header.SenderID);
						break;
					}
					ConnectionMgr->AssignOwnerID(connection, msg.Header.SenderID);
					LOG_INFO_F("[NetThread] Assigned OwnerID %u to connection %u", msg.Header.SenderID, connection);
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