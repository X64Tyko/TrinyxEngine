#if defined(TNX_ENABLE_NETWORK) && defined(TNX_NET_MODEL_CLIENT)

#include "TestFramework.h"
#include "EngineConfig.h"
#include "GNSContext.h"
#include "ClientNetThread.h"
#include "Logger.h"

#include <SDL3/SDL_timer.h>
#include <cstdlib>
#include <memory>

// Two-process loopback: CLIENT side.
//
// Runs ClientNetThread with Start() — the full ThreadMain loop runs on its own thread,
// exercising the complete message-pump and HandleMessage dispatch path.
//
// Flow tested:
//   Connect → GNS connection established → OnConnectionStatusChanged auto-sends ConnectionHandshake
//   → ThreadMain polls + dispatches → HandleMessage(ConnectionHandshake) receives server's
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

	// Heap-allocate so we control destruction order — RAII stopper ensures
	// the thread is always stopped before ClientNetThread destructs, even if
	// an ASSERT fires (which throws std::runtime_error and unwinds the stack).
	auto clientThread = std::make_unique<ClientNetThread>();
	clientThread->Initialize(&gns, &config);

	// Start the real ThreadMain — pumps RunCallbacks+PollIncoming+HandleMessage
	// at InputNetHz, exactly as in production.
	clientThread->Start();

	// RAII guard: always Stop+Join before clientThread destructs.
	struct ThreadStopper
	{
		ClientNetThread* T;
		~ThreadStopper() { T->Stop(); T->Join(); }
	} stopper{clientThread.get()};

	constexpr uint16_t kPort       = 27020;
	HSteamNetConnection serverConn = clientThread->GetConnectionManager()->Connect("127.0.0.1", kPort);
	ASSERT(serverConn != 0);
	LOG_ENG_ALWAYS("[Net_Loopback_Client] Connecting to 127.0.0.1:27020...");

	// Poll until the full handshake completes — GetLocalOwnerID() is atomic,
	// safe to read from test thread while ClientNetThread::ThreadMain runs.
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
