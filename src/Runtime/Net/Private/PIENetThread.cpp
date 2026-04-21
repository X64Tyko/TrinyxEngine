#ifdef TNX_ENABLE_EDITOR
#include "PIENetThread.h"

#include "FlowManager.h"
#include "NetConnectionManager.h"
#include "Logger.h"
#include <SDL3/SDL_timer.h>

void PIENetThread::InitChildren()
{
	Authority.InitAsHandler(GNS, Config, ConnectionMgr);
	Authority.BindSoulCallbacks();
}

void PIENetThread::SetAuthorityWorld(World* world)
{
	Authority.SetAuthorityWorld(world);
}

void PIENetThread::SetReplicationSystem(ReplicationSystem* repl)
{
	Authority.SetReplicationSystem(repl);
}

void PIENetThread::AddClient(HSteamNetConnection clientHandle, World* world)
{
	ClientEntry& entry = Clients.emplace_back();
	entry.Handle       = clientHandle;
	entry.OwnerID      = 0; // promoted to real OwnerID in UpdateClientOwnerID after handshake
	entry.OwnerWorld  = world;
	entry.Handler      = std::make_unique<OwnerNetThread>();
	entry.Handler->InitAsHandler(GNS, Config, ConnectionMgr);
	// Register under slot 0 immediately so TravelNotify/ServerReady handlers can
	// find the world before UpdateClientOwnerID promotes the real ownerID slot.
	entry.Handler->SetOwnerWorld(0, world);
}

void PIENetThread::UpdateClientOwnerID(HSteamNetConnection clientHandle, uint8_t ownerID, World* world)
{
	for (auto& entry : Clients)
	{
		if (entry.Handle == clientHandle)
		{
			entry.OwnerID     = ownerID;
			entry.OwnerWorld = world;
			entry.Handler->SetOwnerWorld(ownerID, world);
			MapConnectionToWorld(ownerID, world);
			LOG_ENG_INFO_F("[PIENet] Client handle %u promoted to OwnerID=%u", clientHandle, ownerID);
			return;
		}
	}
	LOG_ENG_WARN_F("[PIENet] UpdateClientOwnerID: no entry for handle %u", clientHandle);
}

void PIENetThread::RemoveClient(uint8_t ownerID)
{
	auto it = std::find_if(Clients.begin(), Clients.end(),
						   [ownerID](const ClientEntry& e) { return e.OwnerID == ownerID; });
	if (it != Clients.end()) Clients.erase(it);
	MapConnectionToWorld(ownerID, nullptr);
}

void PIENetThread::ClearClients()
{
	Clients.clear();
}

void PIENetThread::TickReplication()
{
	Authority.TickReplication();

	// Compute dt from SDL perf counter — same source as TrinyxEngine's Sentinel loop.
	const uint64_t now = SDL_GetPerformanceCounter();
	const float dt     = LastFlowTickTime
						 ? static_cast<float>(static_cast<double>(now - LastFlowTickTime)
							 / static_cast<double>(SDL_GetPerformanceFrequency()))
						 : 0.0f;
	LastFlowTickTime = now;

	// Tick the server's FlowManager.
	World* serverWorld = Authority.GetAuthorityWorld();
	if (serverWorld) if (FlowManager* flow = serverWorld->GetFlowManager()) flow->Tick(dt);

	// Tick each client's FlowManager and drain deferred replication work.
	for (auto& entry : Clients)
	{
		if (entry.OwnerWorld) if (FlowManager* flow = entry.OwnerWorld->GetFlowManager()) flow->Tick(dt);
		entry.Handler->TickReplication();
	}
}

void PIENetThread::TickInputSend()
{
	for (auto& entry : Clients) entry.Handler->TickInputSend();
}

void PIENetThread::HandleMessage(const ReceivedMessage& msg)
{
	const ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);

	if (!ci)
	{
		LOG_ENG_WARN_F("[PIENet] HandleMessage: unknown connection %u", msg.Connection);
		return;
	}

	if (ci->bAuthoritySide)
	{
		Authority.HandleMessage(msg);
	}
	else
	{
		for (auto& entry : Clients)
		{
			// Always match on the registered client-side handle — the client-initiated CI
			// may still have OwnerID=0 when the first post-handshake messages arrive.
			if (entry.Handle == msg.Connection)
			{
				entry.Handler->HandleMessage(msg);
				return;
			}
		}

		LOG_ENG_WARN_F("[PIENet] HandleMessage: no client handler for connection %u (OwnerID=%u)",
					   msg.Connection, ci->OwnerID);
	}
}

#endif // TNX_ENABLE_EDITOR
