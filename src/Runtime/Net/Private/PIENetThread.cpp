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
	MapConnectionToWorld(0, world);
	Server.MapConnectionToWorld(0, world);
}
void PIENetThread::SetServerFlow(FlowManager* flow)
{
	Server.SetFlowManager(flow);
}

void PIENetThread::SetReplicationSystem(ReplicationSystem* repl)
{
	Server.SetReplicationSystem(repl);
}

void PIENetThread::AddClient(uint8_t ownerID, World* world, FlowManager* flow)
{
	ClientEntry& entry = Clients.emplace_back();
	entry.OwnerID      = ownerID;
	entry.Handler.InitAsHandler(GNS, Config, ConnectionMgr);
	entry.Handler.SetFlowManager(flow);
	entry.Handler.SetClientWorld(ownerID, world);
	MapConnectionToWorld(ownerID, world);
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
			ConnectionInfo* cci = ConnectionMgr->FindConnection(msg.Connection);
			if (cci && cci->OwnerID == entry.OwnerID)
			{
				entry.Handler.HandleMessage(msg);
				return;
			}
		}

		// Client connection not yet assigned an OwnerID (handshake in progress) —
		// route to first client handler as a best-effort fallback.
		if (!Clients.empty())
			Clients[0].Handler.HandleMessage(msg);
	}
}

#endif // TNX_ENABLE_EDITOR
