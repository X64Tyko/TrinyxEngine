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

	void ClearDirty()
	{
		bDirty             = false;
		EarliestDirtyFrame = UINT32_MAX;
	}

	/// Must be called before use. Allocates Depth slots.
	void Initialize(uint32_t temporalFrameCount)
	{
		Depth   = temporalFrameCount;
		Entries = std::make_unique<PlayerInputLogEntry[]>(Depth);
	}

	/// Called by NetThread when an InputFrame arrives.
	/// Payload carries client-local frame numbers. FrameOffset is applied internally
	/// to translate to server-frame space before ring indexing.
	/// For frames not yet consumed: store normally (first-write-wins with out-of-order correction).
	/// For frames already consumed as predicted: compare and mark dirty if different.
	void Store(const InputWindowPacket& payload)
	{
		if (!Entries || payload.FrameCount == 0) return;

		// Translate the incoming window from client-local to server-frame space.
		const uint32_t firstFrame = static_cast<uint32_t>(static_cast<int64_t>(payload.FirstFrame) + FrameOffset);
		const uint32_t lastFrame  = firstFrame + payload.FrameCount - 1;

		//LOG_ENG_INFO_F("[PlayerInputLog] Storing %u input frames (first=%u, last=%u)", payload.FrameCount, payload.FirstFrame, payload.FirstFrame + payload.FrameCount - 1);

		// First packet activates the log.
		if (!bActive)
		{
			const uint32_t seed = firstFrame > 0 ? firstFrame - 1 : 0;
			LastReceivedFrame   = seed;
			LastConsumedFrame   = seed;
			HighWaterFirstFrame = seed;
			bActive             = true;
		}

		if (lastFrame < HighWaterFirstFrame) return;

		if (firstFrame > HighWaterFirstFrame) HighWaterFirstFrame = firstFrame;

		const uint32_t effectiveFirst = [&]() -> uint32_t
		{
			uint32_t first = std::max(firstFrame, HighWaterFirstFrame);
			if (LastConsumedFrame >= Depth) first = std::max(first, LastConsumedFrame - Depth + 1);
			return first;
		}();

		// No upper cap on the store loop — the ring uses modular indexing (frame % Depth),
		// so storing ahead is safe. Capping at safeLastFrame (LastConsumedFrame + Depth)
		// caused data loss: once LastReceivedFrame advances and the client drops acked frames,
		// those out-of-window frames will never be resent, leaving permanent gaps.
		for (uint32_t frame = effectiveFirst; frame <= lastFrame; ++frame)
		{
			const uint32_t windowIdx = frame - firstFrame;
			if (windowIdx >= payload.FrameCount) break;

			const NetInputFrame& src   = payload.Frames[windowIdx];
			PlayerInputLogEntry& entry = Entries[frame % Depth];

			if (frame <= LastConsumedFrame)
			{
				if (entry.SimFrame != frame || !entry.bPredicted) continue;

				const bool keystateChanged = (std::memcmp(entry.State.KeyState, src.State.KeyState, 64) != 0)
					|| (entry.State.MouseDX != src.State.MouseDX)
					|| (entry.State.MouseDY != src.State.MouseDY)
					|| (entry.State.MouseButtons != src.State.MouseButtons);

				const bool eventsChanged = (src.EventCount != entry.EventCount)
					|| (src.EventCount > 0
						&& std::memcmp(src.Events, entry.Events, src.EventCount * sizeof(NetInputEvent)) != 0);

				// Always confirm as real — even a matching prediction must shed bPredicted=true
				// so ConsumeFrame returns Hit during resim instead of LateOrAliased.
				entry.bPredicted = false;

				if (keystateChanged || eventsChanged)
				{
					entry.State      = src.State;
					entry.EventCount = src.EventCount;
					std::memcpy(entry.Events, src.Events, src.EventCount * sizeof(NetInputEvent));

					bDirty = true;
					if (frame < EarliestDirtyFrame) EarliestDirtyFrame = frame;
				}
				continue;
			}

			// Normal store path — first-write-wins, out-of-order freshness by lastFrame.
			const bool slotMatchesFrame  = (entry.SimFrame == frame);
			const bool incomingIsFresher = (lastFrame < entry.SnapshotFrame);
			if (slotMatchesFrame && !incomingIsFresher) continue;

			entry.State         = src.State;
			entry.SimFrame      = frame;
			entry.SnapshotFrame = lastFrame;
			entry.bPredicted    = false;
			entry.EventCount    = src.EventCount;
			std::memcpy(entry.Events, src.Events, src.EventCount * sizeof(NetInputEvent));
		}

		// LastReceivedFrame tracks "has the client sent us frames up to here?" —
		// used by the stall check. Always update from the packet's actual last frame.
		if (lastFrame > LastReceivedFrame) LastReceivedFrame = lastFrame;
	}

	/// Called by server LogicThread injector each sim tick.
	/// Hit: advances LastConsumedFrame, returns the real entry.
	/// NotYetReceived (within lead budget): writes a predicted entry from last known state
	///   and returns it — server keeps running, entry is flagged for correction on late arrival.
	/// NotYetReceived (beyond lead budget): caller should stall the sim.
	InputConsumeResult ConsumeFrame(uint32_t frameNumber)
	{
		if (!Entries) return {nullptr, InputMissReason::LateOrAliased};

		PlayerInputLogEntry& entry = Entries[frameNumber % Depth];
		if (entry.SimFrame == frameNumber && !entry.bPredicted)
		{
			if (frameNumber > LastConsumedFrame) LastConsumedFrame = frameNumber;
			return {&entry, InputMissReason::Hit};
		}

		const InputMissReason reason = (frameNumber > LastReceivedFrame)
										   ? InputMissReason::NotYetReceived
										   : InputMissReason::LateOrAliased;

		if (reason == InputMissReason::NotYetReceived)
		{
			// Extrapolate: copy last known state into this slot and mark predicted.
			// Prefer the most recent REAL (non-predicted) entry. Predicted entries carry
			// forward whatever their source held, but if the ring contains a mix of
			// predicted-zeros (written before real data arrived) and real entries, stopping
			// at the first predicted-zero would propagate zeros past the real data.
			const PlayerInputLogEntry* lastReal      = nullptr;
			const PlayerInputLogEntry* lastPredicted = nullptr;
			for (uint32_t f = frameNumber - 1; f != UINT32_MAX && f + Depth >= frameNumber; --f)
			{
				const PlayerInputLogEntry& prev = Entries[f % Depth];
				if (prev.SimFrame != f) continue;
				if (!prev.bPredicted)
				{
					lastReal = &prev;
					break;
				}
				if (!lastPredicted) lastPredicted = &prev;
			}
			const PlayerInputLogEntry* lastKnown = lastReal ? lastReal : lastPredicted;

			entry.SimFrame      = frameNumber;
			entry.SnapshotFrame = UINT32_MAX;
			entry.bPredicted    = true;
			entry.EventCount    = 0; // discrete events are never predicted
			entry.State         = lastKnown ? lastKnown->State : InputSnapshot{};
			// Mouse deltas are per-frame values, not persistent state — zero them so the
			// server doesn't predict the same mouse movement forever and dirty every frame.
			entry.State.MouseDX = 0.f;
			entry.State.MouseDY = 0.f;

			if (frameNumber > LastConsumedFrame) LastConsumedFrame = frameNumber;
			return {&entry, InputMissReason::NotYetReceived};
		}

		return {nullptr, InputMissReason::LateOrAliased};
	}
};
