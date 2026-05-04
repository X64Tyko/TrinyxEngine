#include "GNSContext.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include "Logger.h"

// These two functions are defined in the GNS static library but not declared in
// any public GNS header. Declare them here to avoid modifying vendored files.
extern "C" {
void SteamNetworkingSockets_SetManualPollMode(bool bFlag);
void SteamNetworkingSockets_Poll(int msMaxWaitTime);
}

GNSContext::~GNSContext()
{
	Shutdown();
}

bool GNSContext::Initialize(GNSStatusChangedFn statusFn)
{
	if (bInitialized) return true;

	// If GNS is already running (another GNSContext owns it), borrow the interface
	// without calling Init/Kill. This allows test-local GNSContexts to share the
	// engine's GNS library without tearing it down when they go out of scope.
	SocketsInterface = SteamNetworkingSockets();
	if (!SocketsInterface)
	{
		// GNS not yet running — we initialize and own the lifecycle.
		SteamNetworkingSockets_SetManualPollMode(true);

		SteamNetworkingErrMsg errMsg;
		if (!GameNetworkingSockets_Init(nullptr, errMsg))
		{
			LOG_ENG_ERROR("[GNSContext] GameNetworkingSockets_Init failed");
			LOG_ENG_ERROR(errMsg);
			return false;
		}

		SocketsInterface = SteamNetworkingSockets();
		if (!SocketsInterface)
		{
			LOG_ENG_ERROR("[GNSContext] SteamNetworkingSockets() returned nullptr after init");
			GameNetworkingSockets_Kill();
			return false;
		}

		bOwnsGNS = true;
		LOG_ENG_INFO("[GNSContext] Initialized (owner)");
	}
	else
	{
		LOG_ENG_INFO("[GNSContext] Initialized (borrower — GNS already running)");
	}

	SocketsHandle.Sockets        = SocketsInterface;
	SocketsHandle.bIsInitialized = true;
	bInitialized                 = true;

	if (statusFn)
	{
		SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(statusFn);
	}

	return true;
}

void GNSContext::Poll(int msWait)
{
	SteamNetworkingSockets_Poll(msWait);
}

void GNSContext::Shutdown()
{
	if (!bInitialized) return;

	SocketsHandle.bIsInitialized = false;
	SocketsHandle.Sockets        = nullptr;
	SocketsInterface             = nullptr;
	bInitialized                 = false;

	if (bOwnsGNS)
	{
		bOwnsGNS = false;
		GameNetworkingSockets_Kill();
		LOG_ENG_INFO("[GNSContext] Shutdown (owner — GNS killed)");
	}
	else
	{
		LOG_ENG_INFO("[GNSContext] Shutdown (borrower — GNS kept alive)");
	}
}
