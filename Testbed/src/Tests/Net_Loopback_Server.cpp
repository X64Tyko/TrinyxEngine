#if defined(TNX_ENABLE_NETWORK) && defined(TNX_NET_MODEL_SERVER)

#include "TestFramework.h"
#include "EngineConfig.h"
#include "GNSContext.h"
#include "AuthorityNetThread.h"
#include "Logger.h"

#include <SDL3/SDL_timer.h>
#include <cstdlib>

// Two-process loopback: SERVER side.
//
// Runs AuthorityNetThread in inline (PollAndDispatch) mode — no Start(), the test
// thread drives the loop. Tests the real HandleMessage dispatch path:
//   client connects → auto-sends ConnectionHandshake → AuthorityNetThread assigns OwnerID
//   → sends HandshakeAccept response.
//
// This test only runs when TNX_LOOPBACK_SERVER=1 is set in the environment.
// In the regular CI matrix it skips — a client process must connect for it to succeed.
//
// Paired with: Net_Loopback_Client (built into the TNX_NET_MODEL_CLIENT binary)
// CI job:      network-loopback — builds both binaries, runs server in background,
//              then runs client; asserts both exit with code 0.
TEST(Net_Loopback_Server)
{
	(void)Engine;

	if (!getenv("TNX_LOOPBACK_SERVER"))
		SKIP_TEST("Set TNX_LOOPBACK_SERVER=1 — requires a client process on 127.0.0.1:27020");

	GNSContext gns;
	ASSERT(gns.Initialize());

	EngineConfig config{};
	config.ApplyDefaults();

	AuthorityNetThread serverThread;
	serverThread.Initialize(&gns, &config);

	constexpr uint16_t kPort = 27020;
	ASSERT(serverThread.GetConnectionManager()->Listen(kPort));
	LOG_ENG_ALWAYS("[Net_Loopback_Server] Listening on 27020 — waiting for client connection...");

	// Wait up to 10 s for the client to connect
	const uint64_t connectDeadline = SDL_GetTicks() + 10000;
	while (serverThread.GetConnectionManager()->GetConnectionCount() < 1
		   && SDL_GetTicks() < connectDeadline)
	{
		serverThread.PollAndDispatch();
		SDL_Delay(5);
	}
	ASSERT(serverThread.GetConnectionManager()->GetConnectionCount() >= 1);
	LOG_ENG_ALWAYS("[Net_Loopback_Server] Client connected — waiting for handshake...");

	// The client's auto-sent ConnectionHandshake triggers HandleMessage in AuthorityNetThread
	// which calls GenerateNetID and assigns an OwnerID. Poll until that happens.
	const HSteamNetConnection clientHandle = serverThread.GetConnectionManager()->GetConnections().front().Handle;
	const uint64_t handshakeDeadline       = SDL_GetTicks() + 5000;
	while (SDL_GetTicks() < handshakeDeadline)
	{
		serverThread.PollAndDispatch();
		const ConnectionInfo* ci = serverThread.GetConnectionManager()->FindConnection(clientHandle);
		if (ci && ci->OwnerID != 0) break;
		SDL_Delay(5);
	}

	const ConnectionInfo* ci = serverThread.GetConnectionManager()->FindConnection(clientHandle);
	ASSERT(ci != nullptr);
	ASSERT(ci->OwnerID != 0);     // Server never assigned OwnerID — ConnectionHandshake not received
	ASSERT(ci->OwnerID < MaxOwnerIDs); // OwnerID out of valid range
	LOG_ENG_ALWAYS_F("[Net_Loopback_Server] Handshake complete — assigned OwnerID=%u", ci->OwnerID);

	// Wait for client to close the connection (signals test is done on client side)
	const uint64_t disconnectDeadline = SDL_GetTicks() + 8000;
	while (serverThread.GetConnectionManager()->GetConnectionCount() > 0
		   && SDL_GetTicks() < disconnectDeadline)
	{
		serverThread.PollAndDispatch();
		SDL_Delay(5);
	}

	LOG_ENG_ALWAYS("[Net_Loopback_Server] Two-process handshake loopback SERVER PASSED");
	serverThread.GetConnectionManager()->Shutdown();
}

#endif // TNX_ENABLE_NETWORK && TNX_NET_MODEL_SERVER
