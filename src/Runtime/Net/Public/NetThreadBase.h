#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
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
// Owns the GNS transport, OS thread lifecycle, rate-limited Tick loop, clock
// sync probes, per-client input-frame send, and FPS tracking.
//
// Derived must implement two hooks:
//   void HandleMessage(const ReceivedMessage& msg)   — role-specific routing
//   void TickReplication()                            — no-op on client
//
// Two operating modes (same as before):
//   Threaded  — call Start(); used for Client and ListenServer.
//   Inline    — call Tick() from your own loop; used for dedicated Server and
//               PIE child instances (ticked manually by PIENetThread).
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
	// Children initialized this way must NOT call Start() or Join().
	void InitAsHandler(GNSContext* gns, const EngineConfig* config, NetConnectionManager* sharedMgr);

	void Start();
	void Stop();
	void Join();

	bool IsRunning() const { return bIsRunning.load(std::memory_order_relaxed); }

	void Tick();

	NetConnectionManager* GetConnectionManager() { return ConnectionMgr; }
	const NetConnectionManager* GetConnectionManager() const { return ConnectionMgr; }

	float GetNetFPS() const { return NetFPS.load(std::memory_order_relaxed); }
	float GetNetFrameMs() const { return NetFrameMs.load(std::memory_order_relaxed); }

	// Per-client world routing for input-frame send path.
	// OwnerID 0 = server world. 1-255 = client connections.
	void MapConnectionToWorld(uint8_t ownerID, World* world) { WorldMap[ownerID] = world; }

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
	void ThreadMain();
	void TickClockSync(double nowSec);
	void TickInputSend();
	void TickFPS(double nowSec);

	std::thread Thread;
	std::atomic<bool> bIsRunning{false};

	std::atomic<float> NetFPS{0.0f};
	std::atomic<float> NetFrameMs{0.0f};
	double FpsTimer     = 0.0;
	double LastFPSCheck = 0.0;
	int FpsFrameCount   = 0;
};

// ---------------------------------------------------------------------------
// Implementation (header-only so template is visible to all translation units)
// ---------------------------------------------------------------------------

#include "GNSContext.h"
#include "EngineConfig.h"
#include "World.h"
#include "Input.h"
#include "LogicThread.h"
#include "Profiler.h"
#include "ThreadPinning.h"
#include "Logger.h"

template <typename Derived>
void NetThreadBase<Derived>::Initialize(GNSContext* gns, const EngineConfig* config)
{
	GNS    = gns;
	Config = config;

	OwnedConnectionMgr = std::make_unique<NetConnectionManager>();
	OwnedConnectionMgr->Initialize(gns);
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
void NetThreadBase<Derived>::Start()
{
	bIsRunning.store(true, std::memory_order_release);
	Thread = std::thread(&NetThreadBase::ThreadMain, this);
	TrinyxThreading::PinThread(Thread);
}

template <typename Derived>
void NetThreadBase<Derived>::Stop()
{
	bIsRunning.store(false, std::memory_order_release);
}

template <typename Derived>
void NetThreadBase<Derived>::Join()
{
	if (Thread.joinable()) Thread.join();
}

template <typename Derived>
void NetThreadBase<Derived>::ThreadMain()
{
	const uint64_t perfFrequency = SDL_GetPerformanceFrequency();
	const double stepTime        = Config->GetNetworkStepTime();

	while (bIsRunning.load(std::memory_order_acquire))
	{
		TNX_ZONE_NC("Net_Frame", 0xFF8844);

		const uint64_t frameStart = SDL_GetPerformanceCounter();

		Tick();

		const uint64_t targetTicks = static_cast<uint64_t>(stepTime * static_cast<double>(perfFrequency));
		const uint64_t frameEnd    = frameStart + targetTicks;

		uint64_t now = SDL_GetPerformanceCounter();
		if (frameEnd > now)
		{
			const double remainingSec       = static_cast<double>(frameEnd - now) / static_cast<double>(perfFrequency);
			constexpr double SleepMarginSec = 0.001;
			if (remainingSec > SleepMarginSec)
				SDL_Delay(static_cast<uint32_t>((remainingSec - SleepMarginSec) * 1000.0));

			while (SDL_GetPerformanceCounter() < frameEnd)
			{
				/* busy wait */
			}
		}
	}
}

template <typename Derived>
void NetThreadBase<Derived>::Tick()
{
	TNX_ZONE_N("Net_Tick");

	ConnectionMgr->RunCallbacks();

	std::vector<ReceivedMessage> messages;
	ConnectionMgr->PollIncoming(messages);

	for (const auto& msg : messages)
		Self().HandleMessage(msg);

	Self().TickReplication();

	const double nowSec = static_cast<double>(SDL_GetPerformanceCounter())
		/ static_cast<double>(SDL_GetPerformanceFrequency());

	TickClockSync(nowSec);
	TickInputSend();
	TickFPS(nowSec);
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

template <typename Derived>
void NetThreadBase<Derived>::TickInputSend()
{
	std::vector<HSteamNetConnection> clientHandles;
	for (const auto& ci : ConnectionMgr->GetConnections())
		if (ci.bClientInitiated && ci.bConnected && ci.OwnerID != 0)
			clientHandles.push_back(ci.Handle);

	for (HSteamNetConnection handle : clientHandles)
	{
		ConnectionInfo* ci = ConnectionMgr->FindConnection(handle);
		if (!ci) continue;

		World* world = WorldMap[ci->OwnerID];
		if (!world) continue;

		InputBuffer* netInput = world->GetNetInput();
		netInput->Swap();

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

template <typename Derived>
void NetThreadBase<Derived>::TickFPS(double nowSec)
{
	FpsFrameCount++;
	FpsTimer    += nowSec - LastFPSCheck;
	LastFPSCheck = nowSec;

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
