#pragma once
#include "NetTypes.h"
#include "RegistryTypes.h"
#include <memory>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// PlayerInputLog
//
// Per-player ring buffer mirroring the temporal slab — one entry per sim frame,
// indexed by (frameNumber % Depth) where Depth == TemporalFrameCount.
//
// Frame indexing is 1:1 with the slab: the server can look up a frame's input
// the same way it accesses historical component data.
//
// Store() writes an incoming InputWindowPacket. Each NetInputFrame maps
// exactly 1:1 to a sim frame slot — no FrameUSOffset redistribution math needed.
// Each frame's keystate and events are stored directly from the payload.
//
// A SimFrame discriminator in each entry detects ring-buffer aliasing — if the
// slot's SimFrame doesn't match the requested frame, the slot is stale/empty.
//
// ConsumeFrame() returns an InputConsumeResult whose Reason field distinguishes
// "not yet received" (extrapolate by repeating last state) from "stale/aliased"
// (data loss). Discrete events are never extrapolated.
//
// Late packets (frames already past LastConsumedFrame) trigger a rollback path —
// see ExecuteRollback() (todo: expose-rollback-core).
//
// Lifecycle: one log is created per connected player and destroyed on disconnect.
// Initialize() must be called before any Store/ConsumeFrame calls.
//
// Thread safety: written by NetThread (HandleMessage), read by LogicThread
// (ConsumeFrame). Currently unsynchronized — the two threads operate in separate
// phases. A lightweight spinlock is needed when those phases overlap.
// ---------------------------------------------------------------------------

/// One slot in the log — holds the resolved input for a single sim frame.
struct PlayerInputLogEntry
{
	uint32_t SimFrame = UINT32_MAX; // UINT32_MAX = slot is empty / not yet written
	// The LastClientFrame of the packet whose keystate is stored here.
	// Lower = older snapshot = more accurate for this sim frame.
	// Out-of-order arrivals only overwrite if they carry a fresher (older) snapshot.
	uint32_t SnapshotFrame  = UINT32_MAX;
	InputSnapshot State     = {};
	uint8_t EventCount      = 0;
	bool bPredicted         = false; // true = extrapolated from last known state, not real input
	uint8_t _Pad[2]         = {};
	NetInputEvent Events[8] = {};
};

/// Why ConsumeFrame() returned no entry.
enum class InputMissReason : uint8_t
{
	Hit,            // Entry found — no miss
	NotYetReceived, // frameNumber > LastReceivedFrame — packet hasn't arrived yet; extrapolate by repeating last state
	LateOrAliased,  // frameNumber <= LastReceivedFrame but slot is stale or overwritten — data loss
};

struct InputConsumeResult
{
	const PlayerInputLogEntry* Entry = nullptr;
	InputMissReason Reason           = InputMissReason::Hit;

	explicit operator bool() const { return Entry != nullptr; }
};

struct PlayerInputLog
{
	std::unique_ptr<PlayerInputLogEntry[]> Entries;
	uint32_t Depth               = 0;
	uint32_t LastConsumedFrame   = 0;
	uint32_t LastReceivedFrame   = 0;
	uint32_t HighWaterFirstFrame = 0;

	// Client-to-server frame offset: serverFrame = clientFrame + FrameOffset.
	// Set from ClockSyncPayload.LocalFrameAtHandshake on the server:
	//   FrameOffset = ServerFrameAtHandshake - ClientLocalFrameAtHandshake
	// All public frame numbers in this log are server-frame space.
	// Can be updated by heartbeat as simulation times drift.
	// Signed: the client normally leads the server (InputLead frames), so
	// FrameOffset is typically negative (e.g. -3 at InputLead=3).
	int32_t FrameOffset = 0;

	// Set true when PlayerBeginConfirm is dispatched (RepState → Playing).
	// The injector skips this log until then.
	bool bActive = false;

	// Dirty tracking: set when a real packet corrects a previously predicted frame.
	bool bDirty                 = false;
	uint32_t EarliestDirtyFrame = UINT32_MAX;

	// Stall log rate-limiting: UINT32_MAX = never logged yet (first occurrence always fires).
	uint32_t LastStallLogFrame = UINT32_MAX;

	bool IsDirty() const { return bDirty; }

	void ClearDirty();

	/// Must be called before use. Allocates Depth slots.
	void Initialize(uint32_t temporalFrameCount);

	/// Called by NetThread when an InputFrame arrives.
	/// Payload carries client-local frame numbers. FrameOffset is applied internally
	/// to translate to server-frame space before ring indexing.
	/// For frames not yet consumed: store normally (first-write-wins with out-of-order correction).
	/// For frames already consumed as predicted: compare and mark dirty if different.
	void Store(const InputWindowPacket& payload);

	/// Called by server LogicThread injector each sim tick.
	/// Hit: advances LastConsumedFrame, returns the real entry.
	/// NotYetReceived (within lead budget): writes a predicted entry from last known state
	///   and returns it — server keeps running, entry is flagged for correction on late arrival.
	/// NotYetReceived (beyond lead budget): caller should stall the sim.
	InputConsumeResult ConsumeFrame(uint32_t frameNumber);
};
