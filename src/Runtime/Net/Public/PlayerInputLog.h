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
// Store() splits an incoming InputFramePayload across every sim frame in its
// [FirstClientFrame, LastClientFrame] span. Discrete events are distributed to
// the specific sim frame they occurred in, determined by FrameUSOffset relative
// to the fixed sim frame duration (1,000,000 / FixedUpdateHz μs). The held key
// state applies identically to all frames in the span.
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
	uint32_t SimFrame       = UINT32_MAX; // UINT32_MAX = slot is empty / not yet written
	// The LastClientFrame of the packet whose keystate is stored here.
	// Lower = older snapshot = more accurate for this sim frame.
	// Out-of-order arrivals only overwrite if they carry a fresher (older) snapshot.
	uint32_t SnapshotFrame  = UINT32_MAX;
	uint8_t KeyState[64]    = {};
	float MouseDX           = 0.f;
	float MouseDY           = 0.f;
	uint8_t MouseButtons    = 0;
	uint8_t EventCount      = 0;
	bool bPredicted         = false; // true = extrapolated from last known state, not real input
	uint8_t _Pad            = {};
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
	uint32_t Depth             = 0;
	uint32_t LastConsumedFrame = 0;
	uint32_t LastReceivedFrame = 0;
	uint32_t HighWaterFirstFrame = 0;

	// Set true when PlayerBeginConfirm is dispatched (RepState → Playing).
	// The injector skips this log until then.
	bool     bActive           = false;

	// Dirty tracking: set when a real packet corrects a previously predicted frame.
	bool     bDirty            = false;
	uint32_t EarliestDirtyFrame = UINT32_MAX;

	bool     IsDirty()   const { return bDirty; }
	void     ClearDirty()      { bDirty = false; EarliestDirtyFrame = UINT32_MAX; }

	/// Must be called before use. Allocates Depth slots.
	void Initialize(uint32_t temporalFrameCount)
	{
		Depth   = temporalFrameCount;
		Entries = std::make_unique<PlayerInputLogEntry[]>(Depth);
	}

	/// Called by NetThread when an InputFrame arrives.
	/// For frames not yet consumed: store normally (first-write-wins with out-of-order correction).
	/// For frames already consumed as predicted: compare and mark dirty if different.
	void Store(const InputFramePayload& payload, uint32_t fixedUpdateHz)
	{
		if (!Entries) return;

		// First packet activates the log: seed all counters so the injector's lead gate
		// starts from this frame, not from 0.
		if (!bActive)
		{
			const uint32_t seed = payload.FirstClientFrame > 0 ? payload.FirstClientFrame - 1 : 0;
			LastReceivedFrame   = seed;
			LastConsumedFrame   = seed;
			HighWaterFirstFrame = seed;
			bActive             = true;
		}

		if (payload.LastClientFrame <= LastConsumedFrame) return; // entire span already simulated

		const uint32_t frameTimeUS = 1'000'000u / fixedUpdateHz;

		// Advance watermark — records the furthest-forward window start we've seen.
		// Out-of-order packets are clamped to this so they can't touch frames that a
		// later packet has already superseded.
		if (payload.FirstClientFrame > HighWaterFirstFrame) HighWaterFirstFrame = payload.FirstClientFrame;

		const uint32_t effectiveFirst = std::max(payload.FirstClientFrame, HighWaterFirstFrame);

		for (uint32_t frame = effectiveFirst; frame <= payload.LastClientFrame; ++frame)
		{
			PlayerInputLogEntry& entry = Entries[frame % Depth];

			// Build the incoming event set for this frame so we can compare it.
			const uint32_t windowOffsetStart = (frame - payload.FirstClientFrame) * frameTimeUS;
			const uint32_t windowOffsetEnd   = windowOffsetStart + frameTimeUS;

			if (frame <= LastConsumedFrame)
			{
				// Frame was already simulated. Only care if it was predicted — compare and
				// mark dirty so the server can resim with the real input.
				if (entry.SimFrame != frame || !entry.bPredicted) continue;

				const bool keystateChanged = (std::memcmp(entry.KeyState, payload.KeyState, 64) != 0)
										  || (entry.MouseDX != payload.MouseDX)
										  || (entry.MouseDY != payload.MouseDY)
										  || (entry.MouseButtons != payload.MouseButtons);

				// Check if the incoming event set for this frame differs.
				uint8_t incomingEventCount = 0;
				NetInputEvent incomingEvents[8];
				for (uint8_t e = 0; e < payload.EventCount && incomingEventCount < 8; ++e)
				{
					const uint32_t evOffset = payload.Events[e].FrameUSOffset;
					if (evOffset >= windowOffsetStart && evOffset < windowOffsetEnd)
						incomingEvents[incomingEventCount++] = payload.Events[e];
				}

				const bool eventsChanged = (incomingEventCount != entry.EventCount)
					|| (incomingEventCount > 0
						&& std::memcmp(incomingEvents, entry.Events, incomingEventCount * sizeof(NetInputEvent)) != 0);

				if (keystateChanged || eventsChanged)
				{
					// Overwrite with real data and mark the log dirty for resim.
					std::memcpy(entry.KeyState, payload.KeyState, 64);
					entry.MouseDX      = payload.MouseDX;
					entry.MouseDY      = payload.MouseDY;
					entry.MouseButtons = payload.MouseButtons;
					entry.bPredicted   = false;
					entry.EventCount   = static_cast<uint8_t>(incomingEventCount);
					std::memcpy(entry.Events, incomingEvents, incomingEventCount * sizeof(NetInputEvent));

					bDirty = true;
					if (frame < EarliestDirtyFrame) EarliestDirtyFrame = frame;
				}
				continue;
			}

			// Frame not yet consumed — normal store path.
			// Prefer the packet whose snapshot is oldest (smallest LastClientFrame).
			const bool slotMatchesFrame  = (entry.SimFrame == frame);
			const bool incomingIsFresher = (payload.LastClientFrame < entry.SnapshotFrame);
			if (slotMatchesFrame && !incomingIsFresher) continue;

			std::memcpy(entry.KeyState, payload.KeyState, 64);
			entry.MouseDX       = payload.MouseDX;
			entry.MouseDY       = payload.MouseDY;
			entry.MouseButtons  = payload.MouseButtons;
			entry.SimFrame      = frame;
			entry.SnapshotFrame = payload.LastClientFrame;
			entry.bPredicted    = false;
			entry.EventCount    = 0;

			for (uint8_t e = 0; e < payload.EventCount && entry.EventCount < 8; ++e)
			{
				const uint32_t evOffset = payload.Events[e].FrameUSOffset;
				if (evOffset >= windowOffsetStart && evOffset < windowOffsetEnd)
					entry.Events[entry.EventCount++] = payload.Events[e];
			}
		}

		if (payload.LastClientFrame > LastReceivedFrame) LastReceivedFrame = payload.LastClientFrame;
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
			// Find the most recent real or predicted entry to copy from.
			const PlayerInputLogEntry* lastKnown = nullptr;
			for (uint32_t f = frameNumber - 1; f != UINT32_MAX && f + Depth >= frameNumber; --f)
			{
				const PlayerInputLogEntry& prev = Entries[f % Depth];
				if (prev.SimFrame == f) { lastKnown = &prev; break; }
			}

			entry.SimFrame      = frameNumber;
			entry.SnapshotFrame = UINT32_MAX;
			entry.bPredicted    = true;
			entry.EventCount    = 0; // discrete events are never predicted
			if (lastKnown)
			{
				std::memcpy(entry.KeyState, lastKnown->KeyState, 64);
				entry.MouseDX      = lastKnown->MouseDX;
				entry.MouseDY      = lastKnown->MouseDY;
				entry.MouseButtons = lastKnown->MouseButtons;
			}
			else std::memset(entry.KeyState, 0, 64);

			if (frameNumber > LastConsumedFrame) LastConsumedFrame = frameNumber;
			return {&entry, InputMissReason::NotYetReceived};
		}

		return {nullptr, InputMissReason::LateOrAliased};
	}
};
