#include "ServerClientChannel.h"
#include "NetConnectionManager.h"
#include <cstdint>

void ServerClientChannel::Open(uint8_t ownerID, uint32_t logDepth, ConnectionInfo* ci,
							   NetConnectionManager* mgr, uint32_t entityCapacity)
{
	OwnerID = ownerID;
	CI      = ci;
	Channel = NetChannel(ci, mgr, nullptr);
	InputLog.Initialize(logDepth);
	if (entityCapacity > 0) Replicated.assign(entityCapacity, false);
	else Replicated.clear();
}

void ServerClientChannel::Close()
{
	InputLog = PlayerInputLog{};
	Replicated.clear();
	Channel = NetChannel{};
	CI      = nullptr;
	OwnerID = 0;
}
