#if defined(TNX_ENABLE_NETWORK) && defined(TNX_NET_MODEL_CLIENT)

#include "TestFramework.h"
#include "EngineConfig.h"
#include "GNSContext.h"
#include "OwnerNet.h"
#include "Logger.h"

#include <SDL3/SDL_timer.h>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <thread>

// Two-process loopback: CLIENT side.
//
// Drives OwnerNet from a local poll thread (mirroring how Sentinel drives
// PumpMessages in production — no dedicated net thread in NetThreadBase anymore).
//
// Flow tested:
//   Connect → GNS connection established → OnConnectionStatusChanged auto-sends ConnectionHandshake
//   → poll loop calls PumpMessages → HandleMessage(ConnectionHandshake) receives server's
//   HandshakeAccept → AssignOwnerID(conn, SenderID) → LocalOwnerID set atomically
//
// The test thread polls GetLocalOwnerID() until non-zero, then validates the assignment
// and shuts down.
//
// This test only runs when TNX_LOOPBACK_CLIENT=1 is set in the environment.
// In the regular CI matrix it skips — a server process must be listening for it to succeed.
//
// Paired with: Net_Loopback_Server (built into the TNX_NET_MODEL_SERVER binary)
// CI job:      network-loopback — builds both binaries, runs server in background,
//              then runs client; asserts both exit with code 0.
TEST(Net_Loopback_Client)
{
	(void)Engine;

	if (!getenv("TNX_LOOPBACK_CLIENT"))
		SKIP_TEST("Set TNX_LOOPBACK_CLIENT=1 — requires server process listening on 127.0.0.1:27020");

	// Give the server process a moment to bind before we connect
	SDL_Delay(300);

	GNSContext gns;
	ASSERT(gns.Initialize());

	EngineConfig config{};
	config.ApplyDefaults();

	auto clientThread = std::make_unique<OwnerNet>();
	clientThread->Initialize(&gns, &config);

	// Spin a local poll thread to drive PumpMessages — mirrors Sentinel in production.
	std::atomic<bool> pollRunning{true};
	std::thread pollThread([&]()
	{
		while (pollRunning.load(std::memory_order_acquire))
		{
			clientThread->PumpMessages();
			SDL_Delay(1); // ~1000Hz
		}
	});

	// RAII guard: always stop poll thread before clientThread destructs.
	struct PollStopper
	{
		std::atomic<bool>& Running;
		std::thread& Thread;

		~PollStopper()
		{
			Running.store(false, std::memory_order_release);
			Thread.join();
		}
	} stopper{pollRunning, pollThread};

	constexpr uint16_t kPort       = 27020;
	HSteamNetConnection serverConn = clientThread->GetConnectionManager()->Connect("127.0.0.1", kPort);
	ASSERT(serverConn != 0);
	LOG_ENG_ALWAYS("[Net_Loopback_Client] Connecting to 127.0.0.1:27020...");

	// Poll until the full handshake completes — GetLocalOwnerID() is atomic,
	// safe to read from test thread while OwnerNet::ThreadMain runs.
	// Flow: GNS Connected → auto-sends ConnectionHandshake → server GenerateNetID
	//       → server HandshakeAccept → our HandleMessage → AssignOwnerID → LocalOwnerID set.
	const uint64_t handshakeDeadline = SDL_GetTicks() + 5000;
	while (clientThread->GetConnectionManager()->GetLocalOwnerID() == 0
		   && SDL_GetTicks() < handshakeDeadline)
	{
		SDL_Delay(10);
	}

	const uint8_t ownerID = clientThread->GetConnectionManager()->GetLocalOwnerID();
	ASSERT(ownerID != 0);       // Handshake timed out — OwnerID never assigned by server
	ASSERT(ownerID < MaxOwnerIDs); // OwnerID out of valid range
	LOG_ENG_ALWAYS_F("[Net_Loopback_Client] Handshake complete — assigned OwnerID=%u", ownerID);

	// Verify the connection record also reflects the assignment
	const ConnectionInfo* ci = clientThread->GetConnectionManager()->FindConnection(serverConn);
	ASSERT(ci != nullptr);
	ASSERT_EQ(static_cast<int>(ci->OwnerID), static_cast<int>(ownerID));

	// Signal the server we're done by closing the connection.
	// ThreadStopper then calls Stop()+Join() as we exit this scope.
	clientThread->GetConnectionManager()->CloseConnection(serverConn, "Test complete");

	LOG_ENG_ALWAYS("[Net_Loopback_Client] Two-process handshake loopback CLIENT PASSED");
}

#endif // TNX_ENABLE_NETWORK && TNX_NET_MODEL_CLIENT
