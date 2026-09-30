#include "PlayerInputLog.h"

#include <algorithm>
#include <cstring>

void PlayerInputLog::ClearDirty()
{
	bDirty             = false;
	EarliestDirtyFrame = UINT32_MAX;
}

void PlayerInputLog::Initialize(uint32_t temporalFrameCount)
{
	Depth   = temporalFrameCount;
	Entries = std::make_unique<PlayerInputLogEntry[]>(Depth);
}

void PlayerInputLog::Store(const InputWindowPacket& payload)
{
	if (!Entries || payload.FrameCount == 0) return;

	// Translate the incoming window from client-local to server-frame space.
	const uint32_t firstFrame = static_cast<uint32_t>(static_cast<int64_t>(payload.FirstFrame) + FrameOffset);
	const uint32_t lastFrame  = firstFrame + payload.FrameCount - 1;

	// LOG_ENG_INFO_F("[PlayerInputLog] Storing %u input frames (first=%u, last=%u)", payload.FrameCount, payload.FirstFrame, payload.FirstFrame + payload.FrameCount - 1);

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
										 || (entry.State.MouseButtons != src.State.MouseButtons)
										 || (entry.State.ViewYaw != src.State.ViewYaw)
										 || (entry.State.ViewPitch != src.State.ViewPitch);

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

InputConsumeResult PlayerInputLog::ConsumeFrame(uint32_t frameNumber)
{
	if (!Entries) return { nullptr, InputMissReason::LateOrAliased };

	PlayerInputLogEntry& entry = Entries[frameNumber % Depth];
	if (entry.SimFrame == frameNumber && !entry.bPredicted)
	{
		if (frameNumber > LastConsumedFrame) LastConsumedFrame = frameNumber;
		return { &entry, InputMissReason::Hit };
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
		return { &entry, InputMissReason::NotYetReceived };
	}

	return { nullptr, InputMissReason::LateOrAliased };
}
