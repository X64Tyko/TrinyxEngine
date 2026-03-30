#include "NetThread.h"
#include "GNSContext.h"
#include "NetConnectionManager.h"
#include "EngineConfig.h"
#include "Input.h"
#include "Logger.h"
#include "Profiler.h"
#include "ThreadPinning.h"
#include "World.h"

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
				simInput->InjectState(payload->KeyState, payload->MouseDX, payload->MouseDY);
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
				// TODO: Update RTT estimation from timestamp delta
				break;
			}

		case NetMessageType::ConnectionHandshake:
		case NetMessageType::StateCorrection:
		case NetMessageType::EntitySpawn:
		case NetMessageType::EntityDestroy:
			{
				// TODO: Phase 4+ message handling
				break;
			}

		default: LOG_WARN_F("[NetThread] Unknown message type %u", msg.Header.Type);
			break;
	}
}