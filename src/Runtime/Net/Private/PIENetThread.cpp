#ifdef TNX_ENABLE_EDITOR
#include "PIENetThread.h"

#include "NetConnectionManager.h"
#include "Logger.h"

void PIENetThread::InitChildren()
{
	Server.InitAsHandler(GNS, Config, ConnectionMgr);
}

void PIENetThread::SetServerWorld(World* world)
{
	Server.SetServerWorld(world);
}
void PIENetThread::SetServerFlow(FlowManager* flow)
{
	Server.SetFlowManager(flow);
}

void PIENetThread::SetReplicationSystem(ReplicationSystem* repl)
{
	Server.SetReplicationSystem(repl);
}

void PIENetThread::AddClient(HSteamNetConnection clientHandle, World* world, FlowManager* flow)
{
	ClientEntry& entry = Clients.emplace_back();
	entry.Handle       = clientHandle;
	entry.OwnerID      = 0; // promoted to real OwnerID in UpdateClientOwnerID after handshake
	entry.Handler      = std::make_unique<ClientNetThread>();
	entry.Handler->InitAsHandler(GNS, Config, ConnectionMgr);
	entry.Handler->SetFlowManager(flow);
	// World and OwnerID wired in UpdateClientOwnerID once server assigns the ID
	(void)world;
}

void PIENetThread::UpdateClientOwnerID(HSteamNetConnection clientHandle, uint8_t ownerID, World* world)
{
	for (auto& entry : Clients)
	{
		if (entry.Handle == clientHandle)
		{
			entry.OwnerID = ownerID;
			entry.Handler->SetClientWorld(ownerID, world);
			MapConnectionToWorld(ownerID, world);
			LOG_INFO_F("[PIENet] Client handle %u promoted to OwnerID=%u", clientHandle, ownerID);
			return;
		}
	}
	LOG_WARN_F("[PIENet] UpdateClientOwnerID: no entry for handle %u", clientHandle);
}

void PIENetThread::RemoveClient(uint8_t ownerID)
{
	auto it = std::find_if(Clients.begin(), Clients.end(),
		[ownerID](const ClientEntry& e) { return e.OwnerID == ownerID; });
	if (it != Clients.end())
		Clients.erase(it);
	MapConnectionToWorld(ownerID, nullptr);
}

void PIENetThread::ClearClients()
{
	Clients.clear();
}

void PIENetThread::TickReplication()
{
	Server.TickReplication();
}

void PIENetThread::HandleMessage(const ReceivedMessage& msg)
{
	const ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);

	if (!ci)
	{
		LOG_WARN_F("[PIENet] HandleMessage: unknown connection %u", msg.Connection);
		return;
	}

	if (ci->bServerSide)
	{
		Server.HandleMessage(msg);
	}
	else
	{
		for (auto& entry : Clients)
		{
			// Pre-handshake: OwnerID not yet assigned — route by connection handle.
			// Post-handshake: route by OwnerID.
			const bool match = (entry.OwnerID != 0 && entry.OwnerID == ci->OwnerID)
				|| (entry.OwnerID == 0 && entry.Handle == msg.Connection);
			if (match)
			{
				entry.Handler->HandleMessage(msg);
				return;
			}
		}

		LOG_WARN_F("[PIENet] HandleMessage: no client handler for connection %u (OwnerID=%u)",
				   msg.Connection, ci->OwnerID);
	}
}

#endif // TNX_ENABLE_EDITOR
