#pragma once
#include "Audio.h"

// Engine-internal only. Not part of the public Audio API.
// Only TrinyxEngine should include this header.
namespace Audio
{
	inline void SetManager(AudioManager* mgr) noexcept { Detail::ManagerPtr() = mgr; }
}
