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

/// @brief Owner-side (client) network handler.
///
/// Routes inbound messages: @c ConnectionHandshake, @c Ping/Pong, @c ClockSync,
/// @c TravelNotify, @c FlowEvent, @c EntitySpawn, @c ConstructSpawn,
/// @c StateCorrection, @c EntityDelta.
///
/// @c FlowManager is resolved from the client World on each use — there is no
/// separate @c FlowMgr pointer to keep in sync.
class OwnerNet : public NetThreadBase<OwnerNet>
{
	friend class NetThreadBase<OwnerNet>;

public:
	/// @brief Bind an owner world for @c ownerID. Non-owning; required for EntitySpawn routing.
	void SetOwnerWorld(uint8_t ownerID, WorldBase* world) { MapConnectionToWorld(ownerID, world); }

	/// @brief Send one @c InputFrame packet to the server for each active connection.
	///
	/// Dispatches a General-queue job so Sentinel returns immediately.
	/// Safe to call at 128 Hz — skips dispatch if the previous job is still running.
	void TickInputSend();

	/// @brief Send @c LevelReady to all connections currently in @c LevelLoading state.
	/// Called by StreamingManager via FlowManager once the background level load job completes.
	void AcknowledgeLevelReady(StreamingRequestID requestID);

	/// @brief Drain deferred @c ConstructSpawn payloads that were waiting for their @c EntitySpawn to arrive first.
	void TickReplication();

	/// @brief Route one inbound network message.
	void HandleMessage(const ReceivedMessage& msg);

private:
	static void WriteEntitySpawnFields(Registry* reg, EntityRecord* record,
		const EntitySpawnPayload& payload,
		uint32_t temporalFrame, uint32_t volatileFrame);
	static void HandleEntitySpawn(Registry* reg, const EntitySpawnPayload& payload, uint32_t frame);
	static void HandleStateCorrections(Registry* reg, const StateCorrectionEntry* entries,
		uint32_t count, uint32_t clientFrame,
		WorldBase* world, uint32_t lastAckedFrame);
	static void HandleEntityDelta(Registry* reg, const uint8_t* payload, uint32_t size);
	static void HandleEntityActivate(Registry* reg, const uint32_t* netHandles, uint32_t count, uint32_t frame);
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
