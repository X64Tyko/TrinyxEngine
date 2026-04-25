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

	// Manual poll mode must be set before GameNetworkingSockets_Init to prevent
	// GNS from spawning its background service thread. All I/O is driven inline
	// via Poll() from the NetThread.
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

	SocketsHandle.Sockets        = SocketsInterface;
	SocketsHandle.bIsInitialized = true;

	// Register global connection status callback if provided
	if (statusFn)
	{
		SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(statusFn);
	}

	bInitialized = true;
	LOG_ENG_INFO("[GNSContext] Initialized");
	return true;
}

void GNSContext::Poll(int msWait)
{
	SteamNetworkingSockets_Poll(msWait);
}

void GNSContext::Shutdown()
{
	if (!bInitialized) return;

	SocketsHandle.Sockets        = nullptr;
	SocketsHandle.bIsInitialized = false;
	
	SocketsInterface = nullptr;
	GameNetworkingSockets_Kill();
	bInitialized = false;

	LOG_ENG_INFO("[GNSContext] Shutdown");
}
