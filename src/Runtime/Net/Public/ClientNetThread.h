#pragma once
#include "NetThreadBase.h"
#include <cstdint>
#include <vector>

class World;

// ---------------------------------------------------------------------------
// ClientNetThread
//
// Handles all client-side message routing:
//   ConnectionHandshake (client receive)  Ping/Pong  Pong
//   ClockSync (client compute InputLead)  TravelNotify  FlowEvent
//   EntitySpawn  ConstructSpawn  StateCorrection
//
// FlowManager is resolved from the client World (world->GetFlowManager()) so
// there is no separate FlowMgr pointer to keep in sync.
// ---------------------------------------------------------------------------
class ClientNetThread : public NetThreadBase<ClientNetThread>
{
	friend class NetThreadBase<ClientNetThread>;

public:
	/// Non-owning. Required for EntitySpawn and StateCorrection routing.
	void SetClientWorld(uint8_t ownerID, World* world) { MapConnectionToWorld(ownerID, world); }

	/// Send one InputFrame packet to the server for each active client connection.
	/// Called from NetThreadBase::ThreadMain at InputNetHz (fast path).
	void TickInputSend();

	/// Drain deferred ConstructSpawn payloads that were waiting for EntitySpawn to land.
	void TickReplication();
	void HandleMessage(const ReceivedMessage& msg);

private:
	struct DeferredEntitySpawn
	{
		uint8_t OwnerID;
		std::vector<uint8_t> Payload;
	};

	struct DeferredConstructSpawn
	{
		uint8_t OwnerID;
		std::vector<uint8_t> Payload;
	};

	/// Flush deferred entity spawns.
	void FlushDeferredEntitySpawns();

	/// Attempt to spawn one deferred construct payload. Returns true if done (success or permanent failure).
	bool TrySpawnDeferred(const DeferredConstructSpawn& entry);

	// EntitySpawns are deferred so HandleMessage never blocks; TickReplication drains them first.
	std::vector<DeferredEntitySpawn> DeferredEntitySpawns;
	std::vector<DeferredConstructSpawn> DeferredConstructSpawns;
};
