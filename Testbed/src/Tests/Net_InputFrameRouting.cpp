#ifdef TNX_ENABLE_NETWORK

#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "World.h"
#include "Logger.h"

#include "GNSContext.h"
#include "NetConnectionManager.h"
#include "ServerNetThread.h"
#include "EngineConfig.h"

#include <SDL3/SDL_timer.h>
#include <cstring>

// Validates InputFrame routing from a GNS client through a ServerNetThread into
// the World's InputBuffer. Tests: inline Tick(), InputFrame deserialization, InputBuffer injection.
// Requires: TNX_ENABLE_NETWORK=ON
TEST(Net_InputFrameRouting)
{
	(void)Engine;

	GNSContext gnsLocal;
	ASSERT(gnsLocal.Initialize());

	EngineConfig config{};
	config.NetworkUpdateHz = 30;
	config.ApplyDefaults();

	ServerNetThread net;
	net.Initialize(&gnsLocal, &config);

	World* world = Engine.GetDefaultWorld();
	ASSERT(world != nullptr);
	net.MapConnectionToWorld(1, world);

	NetConnectionManager* mgr = net.GetConnectionManager();

	constexpr uint16_t kPort = 27016;
	ASSERT(mgr->Listen(kPort));

	HSteamNetConnection clientConn = mgr->Connect("127.0.0.1", kPort);
	ASSERT(clientConn != 0);

	const uint64_t deadline = SDL_GetTicks() + 2000;
	while (mgr->GetConnectionCount() < 2 && SDL_GetTicks() < deadline)
	{
		mgr->RunCallbacks();
		SDL_Delay(10);
	}
	ASSERT(mgr->GetConnectionCount() >= 2);

	HSteamNetConnection serverSideConn = 0;
	for (const auto& ci : mgr->GetConnections())
	{
		if (ci.Handle != clientConn) { serverSideConn = ci.Handle; break; }
	}
	ASSERT(serverSideConn != 0);
	mgr->AssignOwnerID(serverSideConn, 1);

	// Build InputFrame with W key pressed (SDL_SCANCODE_W = 26)
	InputFramePayload payload{};
	memset(payload.State.KeyState, 0, 64);
	constexpr uint8_t wScancode            = 26;
	payload.State.KeyState[wScancode >> 3] |= (1u << (wScancode & 7));
	payload.State.MouseDX                  = 1.5f;
	payload.State.MouseDY                  = -0.5f;

	PacketHeader header{};
	header.Type        = static_cast<uint8_t>(NetMessageType::InputFrame);
	header.Flags       = PacketFlag::DefaultFlags;
	header.PayloadSize = sizeof(InputFramePayload);
	header.SequenceNum = 1;
	header.FrameNumber = 0;
	header.SenderID    = 1;

	ASSERT(mgr->Send(clientConn, header, reinterpret_cast<const uint8_t*>(&payload), true));

	// Pump until the key appears in the World's SimInput
	const uint64_t deadline2 = SDL_GetTicks() + 2000;
	bool received             = false;
	while (!received && SDL_GetTicks() < deadline2)
	{
		net.Tick();
		InputBuffer* simInput = world->GetSimInput();
		simInput->Swap();
		if (simInput->IsKeyDown(static_cast<SDL_Scancode>(wScancode)))
			received = true;
		else
			SDL_Delay(10);
	}
	ASSERT(received);

	LOG_ENG_ALWAYS("[Net_InputFrameRouting] InputFrame routed through NetThread to InputBuffer — verified");

	mgr->StopListening();
}

#endif // TNX_ENABLE_NETWORK
