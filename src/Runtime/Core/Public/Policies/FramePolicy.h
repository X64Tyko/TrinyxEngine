#pragma once
#include <cstdint>

/** @addtogroup core
 *  @{
 */

/**
 * @brief TFrameMode policy tag for `LogicThread` — pure tag types, no methods.
 *
 * `IsEditor` drives `if constexpr` branches in LogicThread to enable editor-only
 * paths (PIE scene snapshot/restore, tick-pause) without runtime overhead in
 * shipping builds.
 */

/**
 * @brief Standard gameplay frame; editor paths compiled out.
 */
struct GameFrame
{
	static constexpr bool IsEditor = false; ///< Disables all editor-only LogicThread branches.
};

/**
 * @brief Editor/PIE frame; unlocks scene snapshot, tick-pause, and PIE hooks.
 */
struct EditorFrame
{
	static constexpr bool IsEditor = true; ///< Enables editor-only LogicThread branches.
};

/** @} */
