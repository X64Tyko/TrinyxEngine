#pragma once

#include <cstdint>

#include "Events.h"
#include "Types.h"

// Fixed engine enum controlling Construct tick execution order.
// Not extensible by games — keeps sort order compile-time deterministic.
// Use int16_t OrderWithinGroup for fine-grained ordering within a group.
enum class TickGroup : uint8_t
{
	PreInput    = 0, // Read input state before anything reacts
	Default     = 1, // Standard gameplay logic
	PostDefault = 2, // Things that depend on Default having run
	Camera      = 3, // Camera always resolves after gameplay
	Late        = 4, // Final adjustments, IK, procedural
};

DEFINE_CALLBACK(ConstructTickFn, SimFloat)

struct ConstructTickEntry
{
	ConstructTickFn   Tick;
	TickGroup         Group;
	int16_t           OrderWithinGroup;
};
