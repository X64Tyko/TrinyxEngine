#pragma once
#include "NetThreadBase.h"
#include "TrinyxJobs.h"
#include <cstdint>
#include <vector>

class World;

// ---------------------------------------------------------------------------
// OwnerNetThread
//
// Handles all client-side message routing:
//   ConnectionHandshake (client receive)  Ping/Pong  Pong
//   ClockSync (client compute InputLead)  TravelNotify  FlowEvent
//   EntitySpawn  ConstructSpawn  StateCorrection
//
// FlowManager is resolved from the client World (world->GetFlowManager()) so
// there is no separate FlowMgr pointer to keep in sync.
// ---------------------------------------------------------------------------
class OwnerNetThread : public NetThreadBase<OwnerNetThread>
{
	friend class NetThreadBase<OwnerNetThread>;

public:
	/// Non-owning. Required for EntitySpawn and StateCorrection routing.
	void SetOwnerWorld(uint8_t ownerID, World* world) { MapConnectionToWorld(ownerID, world); }

	/// Send one InputFrame packet to the server for each active client connection.
	/// Dispatches a General-queue job so Sentinel returns immediately.
	/// Safe to call at 128Hz — skips dispatch if the previous job hasn't finished.
	void TickInputSend();

	/// Drain deferred ConstructSpawn payloads that were waiting for EntitySpawn to land.
	void TickReplication();
	void HandleMessage(const ReceivedMessage& msg);

private:
	/// Hot-path payload — runs on a worker thread. Owns the actual packet build + send.
	void ExecuteInputSend();

	/// Completion counter for the in-flight InputSend job.
	/// Sentinel checks Value == 0 before dispatching the next job to prevent overlap.
	TrinyxJobs::JobCounter SendCounter;

	struct DeferredEntitySpawn
	{
		uint8_t OwnerID;
		uint32_t ServerSpawnFrame; // PacketHeader.FrameNumber at time of receipt
		std::vector<uint8_t> Payload;
	};

	struct DeferredConstructSpawn
	{
		uint8_t OwnerID;
		uint32_t ServerSpawnFrame; // PacketHeader.FrameNumber at time of receipt (= payload SpawnFrame)
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
