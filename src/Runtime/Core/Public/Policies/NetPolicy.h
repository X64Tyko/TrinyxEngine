#pragma once

/** @addtogroup core
 *  @{
 */

/**
 * @brief TNetMode policy tag for `LogicThread` — controls input injection and replication dispatch.
 *
 * | Policy        | Role                  | Header                         |
 * |---------------|-----------------------|--------------------------------|
 * | `SoloSim`     | Offline, no net       | *(defined here)*               |
 * | `AuthoritySim`| Authority-side (server/Host) | `AuthoritySim.h`        |
 * | `OwnerSim`    | Owner-side (client)   | `OwnerSim.h`                   |
 *
 * Each policy must expose:
 * @code
 *   template <typename TLogic> bool OnSimInput(uint32_t frame, TLogic& logic);
 *   template <typename TLogic> void OnFramePublished(uint32_t frame, TLogic& logic);
 * @endcode
 */

/**
 * @brief Offline net policy — both hooks are no-ops, zero overhead.
 */
struct SoloSim
{
	/**
	 * @brief No-op; solo play has no remote input to inject.
	 * @tparam TLogic Concrete LogicThread specialization.
	 * @return Always `false` — no corrections pending.
	 */
	template <typename TLogic>
	bool OnSimInput(uint32_t /*frame*/, TLogic& /*logic*/)
	{
		return false;
	}

	/**
	 * @brief No-op; solo play has no peers to replicate to.
	 * @tparam TLogic Concrete LogicThread specialization.
	 */
	template <typename TLogic>
	void OnFramePublished(uint32_t /*frame*/, TLogic& /*logic*/)
	{
	}
};

/** @} */
