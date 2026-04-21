#ifdef TNX_ENABLE_NETWORK

#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Logger.h"

#include "GNSContext.h"
#include "NetConnectionManager.h"

#include <SDL3/SDL_timer.h>
#include <vector>

// Validates GNS static linking, loopback connectivity, and PacketHeader framing.
// Requires: TNX_ENABLE_NETWORK=ON
TEST(Net_LoopbackPingPong)
{
	(void)Engine;

	GNSContext gnsLocal;
	ASSERT(gnsLocal.Initialize());

	NetConnectionManager mgr;
	mgr.Initialize(&gnsLocal);

	constexpr uint16_t kPort = 27015;
	ASSERT(mgr.Listen(kPort));

	HSteamNetConnection clientConn = mgr.Connect("127.0.0.1", kPort);
	ASSERT(clientConn != 0);

	// Pump until both sides are connected
	const uint64_t deadline = SDL_GetTicks() + 2000;
	while (mgr.GetConnectionCount() < 2 && SDL_GetTicks() < deadline)
	{
		gnsLocal.Poll();
		mgr.RunCallbacks();
		SDL_Delay(10);
	}
	ASSERT(mgr.GetConnectionCount() >= 2);

	// Drain any handshake messages
	{
		std::vector<ReceivedMessage> drain;
		for (int i = 0; i < 10; ++i)
		{
			gnsLocal.Poll();
			mgr.RunCallbacks();
			mgr.PollIncoming(drain);
			SDL_Delay(5);
		}
	}

	// Identify server-side connection handle
	HSteamNetConnection serverConn = 0;
	for (const auto& ci : mgr.GetConnections())
	{
		if (ci.Handle != clientConn) { serverConn = ci.Handle; break; }
	}
	ASSERT(serverConn != 0);

	// Client sends Ping
	PacketHeader pingHeader{};
	pingHeader.Type        = static_cast<uint8_t>(NetMessageType::Ping);
	pingHeader.Flags       = PacketFlag::DefaultFlags;
	pingHeader.SequenceNum = 1;
	pingHeader.FrameNumber = 0;
	pingHeader.SenderID    = 1;
	pingHeader.Timestamp   = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
	pingHeader.PayloadSize = 0;

	ASSERT(mgr.Send(clientConn, pingHeader, nullptr, true));

	// Poll until Ping arrives on server side
	std::vector<ReceivedMessage> msgs;
	const uint64_t recvDeadline = SDL_GetTicks() + 1000;
	while (msgs.empty() && SDL_GetTicks() < recvDeadline)
	{
		gnsLocal.Poll();
		mgr.RunCallbacks();
		mgr.PollIncoming(msgs);
		SDL_Delay(5);
	}
	ASSERT_EQ(static_cast<int>(msgs.size()), 1);
	ASSERT_EQ(msgs[0].Header.Type, static_cast<uint8_t>(NetMessageType::Ping));
	ASSERT_EQ(msgs[0].Header.SenderID, 1);
	ASSERT_EQ(msgs[0].Connection, serverConn);

	// Server responds with Pong
	PacketHeader pongHeader{};
	pongHeader.Type        = static_cast<uint8_t>(NetMessageType::Pong);
	pongHeader.Flags       = PacketFlag::DefaultFlags;
	pongHeader.SequenceNum = 1;
	pongHeader.FrameNumber = 0;
	pongHeader.SenderID    = 0;
	pongHeader.Timestamp   = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
	pongHeader.PayloadSize = 0;

	ASSERT(mgr.Send(serverConn, pongHeader, nullptr, true));

	// Poll until Pong arrives on client side
	msgs.clear();
	const uint64_t pongDeadline = SDL_GetTicks() + 1000;
	while (msgs.empty() && SDL_GetTicks() < pongDeadline)
	{
		gnsLocal.Poll();
		mgr.RunCallbacks();
		mgr.PollIncoming(msgs);
		SDL_Delay(5);
	}
	ASSERT_EQ(static_cast<int>(msgs.size()), 1);
	ASSERT_EQ(msgs[0].Header.Type, static_cast<uint8_t>(NetMessageType::Pong));
	ASSERT_EQ(msgs[0].Header.SenderID, 0);
	ASSERT_EQ(msgs[0].Connection, clientConn);

	LOG_ENG_ALWAYS("[Net_LoopbackPingPong] Loopback ping/pong successful — GNS connectivity verified");

	mgr.Shutdown();
}

#endif // TNX_ENABLE_NETWORK
