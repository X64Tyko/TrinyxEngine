#pragma once

#include "Types.h"

struct InputBuffer;

// ─── Soul input layer mixins ──────────────────────────────────────────────────
// Inherit one or both on a Soul input layer to opt into sim/viz input ticks.
// The Soul's input stack calls TickSim / TickViz when dispatching input.
//
// sim input  — deterministic, fixed-tick, feeds into PlayerInputLog / rollback
// viz input  — presentation-rate, mouse look, camera, UI — not replayed
//
// Typical use: inherit both on a PlayerInputLayer, call
//   GetSoul()->DispatchOrientationDelta(dx * Sens, -dy * Sens)
// from TickViz to drive the camera, and read action buttons from TickSim
// to drive character movement.

template <typename Derived>
struct SimInputMix
{
	// void TickSim(SimFloat dt, InputBuffer& input);
};

template <typename Derived>
struct VizInputMix
{
	// void TickViz(SimFloat dt, InputBuffer& input);
};
