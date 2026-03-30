#include "GNSContext.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include "Logger.h"

GNSContext::~GNSContext()
{
	Shutdown();
}

bool GNSContext::Initialize(GNSStatusChangedFn statusFn)
{
	if (bInitialized) return true;

	SteamNetworkingErrMsg errMsg;
	if (!GameNetworkingSockets_Init(nullptr, errMsg))
	{
		LOG_ERROR("[GNSContext] GameNetworkingSockets_Init failed");
		LOG_ERROR(errMsg);
		return false;
	}

	SocketsInterface = SteamNetworkingSockets();
	if (!SocketsInterface)
	{
		LOG_ERROR("[GNSContext] SteamNetworkingSockets() returned nullptr after init");
		GameNetworkingSockets_Kill();
		return false;
	}

	// Register global connection status callback if provided
	if (statusFn)
	{
		SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(statusFn);
	}

	bInitialized = true;
	LOG_INFO("[GNSContext] Initialized");
	return true;
}

void GNSContext::Shutdown()
{
	if (!bInitialized) return;

	SocketsInterface = nullptr;
	GameNetworkingSockets_Kill();
	bInitialized = false;

	LOG_INFO("[GNSContext] Shutdown");
}
