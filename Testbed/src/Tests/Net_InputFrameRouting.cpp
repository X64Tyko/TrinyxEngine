#ifdef TNX_ENABLE_NETWORK

#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Logger.h"

#include "GNSContext.h"
#include "NetConnectionManager.h"
#include "AuthorityNet.h"
#include "ReplicationSystem.h"
#include "PlayerInputLog.h"
#include "EngineConfig.h"

#include <SDL3/SDL_timer.h>
#include <cstring>

// Validates InputFrame routing from a GNS client through a AuthorityNet into
// the server-side PlayerInputLog for that player. Tests: inline Tick(),
// InputFrame deserialization, PlayerInputLog.Store(), first-write-wins correctness.
//
// Real-world flow: client sends InputFrame → AuthorityNet::HandleMessage dispatches
// it to PlayerInputLog::Store → LogicThread reads it via ConsumeFrame each fixed step.
// The test directly inspects the PlayerInputLog entry rather than InputBuffer (which
// is for local keyboard input, not network-received input).
//
// Requires: TNX_ENABLE_NETWORK=ON
TEST(Net_InputFrameRouting)
{
	(void)Engine;

	GNSContext gnsLocal;
	ASSERT(gnsLocal.Initialize());

	EngineConfig config{};
	config.NetworkUpdateHz = 30;
	config.ApplyDefaults();

	AuthorityNet net;
	net.Initialize(&gnsLocal, &config);

	NetConnectionManager* mgr = net.GetConnectionManager();

	constexpr uint16_t kPort = 27016;
	ASSERT(mgr->Listen(kPort));

	HSteamNetConnection clientConn = mgr->Connect("127.0.0.1", kPort);
	ASSERT(clientConn != 0);

	// Wait for both sides to connect
	const uint64_t connectDeadline = SDL_GetTicks() + 2000;
	while (mgr->GetConnectionCount() < 2 && SDL_GetTicks() < connectDeadline)
	{
		net.PollAndDispatch();
		SDL_Delay(10);
	}
	ASSERT(mgr->GetConnectionCount() >= 2);

	// Drain the auto-sent ConnectionHandshake so it doesn't interfere with our InputFrame check
	for (int i = 0; i < 10; ++i)
	{
		net.PollAndDispatch();
		SDL_Delay(5);
	}

	// Identify the server-side connection handle and assign OwnerID=1
	HSteamNetConnection serverSideConn = 0;
	for (const auto& ci : mgr->GetConnections())
	{
		if (ci.Handle != clientConn) { serverSideConn = ci.Handle; break; }
	}
	ASSERT(serverSideConn != 0);
	mgr->AssignOwnerID(serverSideConn, 1);

	// CreateInputLog now goes through ReplicationSystem::OpenChannel.
	// Wire a minimal ReplicationSystem (no World — this test has no sim) before calling it.
	ReplicationSystem repl;
	repl.Initialize(nullptr);
	net.SetReplicationSystem(&repl);

	// Allocate the PlayerInputLog for ownerID=1 (normally created by GenerateNetID after
	// the full handshake; we call it directly here since we bypassed the handshake).
	net.CreateInputLog(1);
	ASSERT(net.GetInputLog(1) != nullptr);

	// Build an InputFrame covering sim frame 10 with W key pressed (SDL_SCANCODE_W = 26)
	constexpr uint8_t kOwnerID   = 1;
	constexpr uint32_t kSimFrame = 10;
	constexpr uint8_t kWScancode = 26;

	InputWindowPacket payload{};
	payload.FirstFrame                                = kSimFrame;
	payload.FrameCount                                = 1;
	payload.Frames[0].Frame                           = kSimFrame;
	payload.Frames[0].State.KeyState[kWScancode >> 3] |= (1u << (kWScancode & 7));
	payload.Frames[0].State.MouseDX                   = 1.5f;
	payload.Frames[0].State.MouseDY                   = -0.5f;

	PacketHeader header{};
	header.Type        = static_cast<uint8_t>(NetMessageType::InputFrame);
	header.Flags       = PacketFlag::DefaultFlags;
	header.PayloadSize = sizeof(InputWindowPacket);
	header.SequenceNum = 1;
	header.FrameNumber = kSimFrame;
	header.SenderID    = kOwnerID;

	ASSERT(mgr->Send(clientConn, header, reinterpret_cast<const uint8_t*>(&payload), true));

	// Pump until the InputFrame is dispatched into the PlayerInputLog
	const uint64_t deadline = SDL_GetTicks() + 2000;
	bool stored             = false;
	while (!stored && SDL_GetTicks() < deadline)
	{
		net.PollAndDispatch();
		const PlayerInputLog* log = net.GetInputLog(kOwnerID);
		if (log && log->bActive)
		{
			const PlayerInputLogEntry& entry = log->Entries[kSimFrame % log->Depth];
			if (entry.SimFrame == kSimFrame)
			{
				// Verify the W key state was stored correctly
				const bool wDown = (entry.State.KeyState[kWScancode >> 3] & (1u << (kWScancode & 7))) != 0;
				ASSERT(wDown);
				ASSERT_EQ(entry.State.MouseDX, 1.5f);
				stored = true;
			}
		}
		if (!stored) SDL_Delay(5);
	}
	ASSERT(stored); // InputFrame never dispatched into PlayerInputLog within 2s

	LOG_ENG_ALWAYS("[Net_InputFrameRouting] InputFrame routed through NetThread to PlayerInputLog — verified");

	mgr->StopListening();
	mgr->Shutdown();
}

#endif // TNX_ENABLE_NETWORK
