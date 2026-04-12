#pragma once
#include "NetThreadBase.h"
#include <cstdint>
#include <vector>

class FlowManager;
class World;

// ---------------------------------------------------------------------------
// ClientNetThread
//
// Handles all client-side message routing:
//   ConnectionHandshake (client receive)  Ping/Pong  Pong
//   ClockSync (client compute InputLead)  TravelNotify  FlowEvent
//   EntitySpawn  ConstructSpawn  StateCorrection
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

	/// Drain deferred ConstructSpawn payloads that were waiting for EntitySpawn to land.
	void TickReplication();
	void HandleMessage(const ReceivedMessage& msg);

private:
	struct DeferredConstructSpawn
	{
		uint8_t OwnerID;
		std::vector<uint8_t> Payload;
	};

	/// Attempt to spawn one deferred payload. Returns true if done (success or permanent failure).
	bool TrySpawnDeferred(const DeferredConstructSpawn& entry);

	FlowManager* FlowMgr = nullptr;
	std::vector<DeferredConstructSpawn> DeferredConstructSpawns;
};
