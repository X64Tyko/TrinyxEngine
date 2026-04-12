#pragma once
#include "NetTypes.h"
#include "RegistryTypes.h"
#include <memory>
#include <cstring>

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
	uint8_t KeyState[64]    = {};
	float MouseDX           = 0.f;
	float MouseDY           = 0.f;
	uint8_t MouseButtons    = 0;
	uint8_t EventCount      = 0;
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
	uint32_t Depth             = 0;
	uint32_t LastConsumedFrame = 0; // highest sim frame handed to the simulation
	uint32_t LastReceivedFrame = 0; // highest LastClientFrame stored — used to classify misses

	/// Must be called before use. Allocates Depth slots matching TemporalFrameCount.
	void Initialize(uint32_t temporalFrameCount)
	{
		Depth   = temporalFrameCount;
		Entries = std::make_unique<PlayerInputLogEntry[]>(Depth);
	}

	/// Called by NetThread when an InputFrame arrives.
	/// Splits the payload across every sim frame in [FirstClientFrame, LastClientFrame].
	/// Each frame's slot (frame % Depth) receives: the full held key state, mouse delta,
	/// and the subset of discrete events whose FrameUSOffset falls within that frame's
	/// time window (fixedUpdateHz used to determine per-frame μs duration).
	/// Frames at or below LastConsumedFrame are skipped (already simulated).
	void Store(const InputFramePayload& payload, uint32_t fixedUpdateHz)
	{
		if (!Entries) return;
		if (payload.LastClientFrame <= LastConsumedFrame) return; // entire span already simulated

		const uint32_t frameTimeUS = 1'000'000u / fixedUpdateHz;

		for (uint32_t frame = payload.FirstClientFrame; frame <= payload.LastClientFrame; ++frame)
		{
			if (frame <= LastConsumedFrame) continue; // skip frames already consumed within span

			PlayerInputLogEntry& entry = Entries[frame % Depth];

			// Held state applies uniformly to all frames in the span
			std::memcpy(entry.KeyState, payload.KeyState, 64);
			entry.MouseDX      = payload.MouseDX;
			entry.MouseDY      = payload.MouseDY;
			entry.MouseButtons = payload.MouseButtons;
			entry.SimFrame     = frame;
			entry.EventCount   = 0;

			// Distribute discrete events to the sim frame they occurred in.
			// FrameUSOffset is μs since the input window opened (at FirstClientFrame).
			const uint32_t windowOffsetStart = (frame - payload.FirstClientFrame) * frameTimeUS;
			const uint32_t windowOffsetEnd   = windowOffsetStart + frameTimeUS;

			for (uint8_t e = 0; e < payload.EventCount && entry.EventCount < 8; ++e)
			{
				const uint32_t evOffset = payload.Events[e].FrameUSOffset;
				if (evOffset >= windowOffsetStart && evOffset < windowOffsetEnd) entry.Events[entry.EventCount++] = payload.Events[e];
			}
		}

		if (payload.LastClientFrame > LastReceivedFrame) LastReceivedFrame = payload.LastClientFrame;
	}

	/// Called by LogicThread each sim tick.
	/// On hit: advances LastConsumedFrame and returns the entry with Reason::Hit.
	/// On miss: Reason::NotYetReceived means the packet simply hasn't arrived yet —
	/// extrapolate by repeating the last key state. Reason::LateOrAliased means the
	/// slot was clobbered or the frame was in a received span but overwritten — log
	/// a diagnostic or schedule a resim.
	/// TODO(rollback): Reason::LateOrAliased with frameNumber < LastConsumedFrame means
	/// a late packet arrived for an already-simulated frame — trigger ExecuteRollback().
	InputConsumeResult ConsumeFrame(uint32_t frameNumber)
	{
		if (!Entries) return {nullptr, InputMissReason::LateOrAliased};

		const PlayerInputLogEntry& entry = Entries[frameNumber % Depth];
		if (entry.SimFrame == frameNumber)
		{
			if (frameNumber > LastConsumedFrame) LastConsumedFrame = frameNumber;
			return {&entry, InputMissReason::Hit};
		}

		const InputMissReason reason = (frameNumber > LastReceivedFrame)
										   ? InputMissReason::NotYetReceived
										   : InputMissReason::LateOrAliased;
		return {nullptr, reason};
	}
};
