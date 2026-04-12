#ifdef TNX_ENABLE_EDITOR
#include "PIENetThread.h"

#include "FlowManager.h"
#include "NetConnectionManager.h"
#include "Logger.h"
#include <SDL3/SDL_timer.h>

void PIENetThread::InitChildren()
{
Server.InitAsHandler(GNS, Config, ConnectionMgr);
Server.BindSoulCallbacks();
}

void PIENetThread::PumpMessages()
{
ConnectionMgr->RunCallbacks();

std::vector<ReceivedMessage> messages;
ConnectionMgr->PollIncoming(messages);
for (const auto& msg : messages) HandleMessage(msg);
}

void PIENetThread::SetServerWorld(World* world)
{
Server.SetServerWorld(world);
}

void PIENetThread::SetReplicationSystem(ReplicationSystem* repl)
{
Server.SetReplicationSystem(repl);
}

void PIENetThread::AddClient(HSteamNetConnection clientHandle, World* world)
{
ClientEntry& entry   = Clients.emplace_back();
entry.Handle         = clientHandle;
entry.OwnerID        = 0; // promoted to real OwnerID in UpdateClientOwnerID after handshake
entry.ClientWorld    = world;
entry.Handler        = std::make_unique<ClientNetThread>();
entry.Handler->InitAsHandler(GNS, Config, ConnectionMgr);
// World and OwnerID wired in UpdateClientOwnerID once server assigns the ID
}

void PIENetThread::UpdateClientOwnerID(HSteamNetConnection clientHandle, uint8_t ownerID, World* world)
{
for (auto& entry : Clients)
{
if (entry.Handle == clientHandle)
{
entry.OwnerID     = ownerID;
entry.ClientWorld = world;
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

// Compute dt from SDL perf counter — same source as TrinyxEngine's Sentinel loop.
const uint64_t now  = SDL_GetPerformanceCounter();
const float dt      = LastFlowTickTime
? static_cast<float>(static_cast<double>(now - LastFlowTickTime)
/ static_cast<double>(SDL_GetPerformanceFrequency()))
: 0.0f;
LastFlowTickTime = now;

// Tick the server's FlowManager.
World* serverWorld = Server.GetServerWorld();
if (serverWorld)
if (FlowManager* flow = serverWorld->GetFlowManager()) flow->Tick(dt);

// Tick each client's FlowManager and drain deferred replication work.
for (auto& entry : Clients)
{
if (entry.ClientWorld)
if (FlowManager* flow = entry.ClientWorld->GetFlowManager()) flow->Tick(dt);
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
// Always match on the registered client-side handle — the client-initiated CI
// may still have OwnerID=0 when the first post-handshake messages arrive.
if (entry.Handle == msg.Connection)
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
