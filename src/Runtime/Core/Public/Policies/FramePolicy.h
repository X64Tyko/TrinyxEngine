#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Frame-advance policies for LogicThread<TNetMode, TRollbackMode, TFrameMode>.
//
// Pure tag types — no methods. IsEditor drives if constexpr branches.
// ---------------------------------------------------------------------------

struct GameFrame
{
static constexpr bool IsEditor = false;
};

struct EditorFrame
{
static constexpr bool IsEditor = true;
};
