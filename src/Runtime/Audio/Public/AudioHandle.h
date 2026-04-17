#pragma once
#include <cstdint>

// Lightweight handle to a playing voice. Index + Generation for stale-handle detection.
// SoundHandle::Invalid() = {0, 0}.  Generation is always >= 1 for live handles,
// so {0, 0} can never alias a real slot.
struct SoundHandle
{
	uint16_t Index      = 0; // slot in the voice pool
	uint16_t Generation = 0; // bumped on Stop/reuse

	bool IsValid() const { return Index != 0 || Generation != 0; }

	bool operator==(const SoundHandle& o) const { return Index == o.Index && Generation == o.Generation; }
	bool operator!=(const SoundHandle& o) const { return !(*this == o); }

	static SoundHandle Invalid() { return {0, 0}; }
};
