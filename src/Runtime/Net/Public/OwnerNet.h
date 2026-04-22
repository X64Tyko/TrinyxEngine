#pragma once
#include "NetThreadBase.h"
#include "TrinyxJobs.h"
#include <cstdint>
#include <vector>

class WorldBase;
class Registry;
class ConstructRegistry;
class LogicThreadBase;
struct EntityRecord;
struct EntitySpawnPayload;
struct StateCorrectionEntry;

// ---------------------------------------------------------------------------
// OwnerNet
//
// Handles all client-side message routing:
//   ConnectionHandshake (client receive)  Ping/Pong  Pong
//   ClockSync (client compute InputLead)  TravelNotify  FlowEvent
//   EntitySpawn  ConstructSpawn  StateCorrection
//
// FlowManager is resolved from the client World (world->GetFlowManager()) so
// there is no separate FlowMgr pointer to keep in sync.
// ---------------------------------------------------------------------------
class OwnerNet : public NetThreadBase<OwnerNet>
{
	friend class NetThreadBase<OwnerNet>;

public:
	/// Non-owning. Required for EntitySpawn and StateCorrection routing.
	void SetOwnerWorld(uint8_t ownerID, WorldBase* world) { MapConnectionToWorld(ownerID, world); }

	/// Send one InputFrame packet to the server for each active client connection.
	/// Dispatches a General-queue job so Sentinel returns immediately.
	/// Safe to call at 128Hz — skips dispatch if the previous job hasn't finished.
	void TickInputSend();

	/// Drain deferred ConstructSpawn payloads that were waiting for EntitySpawn to land.
	void TickReplication();
	void HandleMessage(const ReceivedMessage& msg);

private:
	static void WriteEntitySpawnFields(Registry* reg, EntityRecord* record,
									   const EntitySpawnPayload& payload,
									   uint32_t temporalFrame, uint32_t volatileFrame);
	static void HandleEntitySpawn(Registry* reg, const EntitySpawnPayload& payload, uint32_t frame);
	static void HandleStateCorrections(Registry* reg, const StateCorrectionEntry* entries,
									   uint32_t count, uint32_t clientFrame,
									   LogicThreadBase* logic, uint32_t lastAckedFrame);
	static bool HandleConstructSpawn(ConstructRegistry* reg, Registry* entityReg,
									 WorldBase* clientWorld, const uint8_t* data, size_t len);
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
