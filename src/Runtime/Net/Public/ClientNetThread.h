#pragma once
#include "NetThreadBase.h"

class FlowManager;
class World;

// ---------------------------------------------------------------------------
// ClientNetThread
//
// Handles all client-side message routing:
//   ConnectionHandshake (client receive)  Ping/Pong  Pong
//   ClockSync (client compute InputLead)  TravelNotify  FlowEvent
//   EntitySpawn  StateCorrection
//
// Holds a non-owning FlowManager pointer for posting net events to Sentinel.
// FlowManager lives on Sentinel — ClientNetThread never owns it.
// ---------------------------------------------------------------------------
class ClientNetThread : public NetThreadBase<ClientNetThread>
{
	friend class NetThreadBase<ClientNetThread>;

public:
	/// Non-owning. Posts TravelNotify and FlowEvents to client's Sentinel flow.
	void SetFlowManager(FlowManager* flow) { FlowMgr = flow; }

	/// Non-owning. Required for EntitySpawn and StateCorrection routing.
	void SetClientWorld(uint8_t ownerID, World* world) { MapConnectionToWorld(ownerID, world); }

	/// No-op: clients don't replicate.
	void TickReplication() {}
	void HandleMessage(const ReceivedMessage& msg);

private:
	FlowManager* FlowMgr = nullptr;
};
