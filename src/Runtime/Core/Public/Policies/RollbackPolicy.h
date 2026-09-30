#pragma once
#include <cstdint>
#include <queue>
#include <vector>

#include "NetTypes.h"
#include "TrinyxMPMCRing.h"

/** @addtogroup core
 *  @{
 */

class LogicThreadBase;

/**
 * @brief TRollbackMode policy for `LogicThread` — no rollback history, zero overhead.
 *
 * All stubs compile away; LogicThread call sites require no `if constexpr` guards.
 * Games that don't need rollback pay no memory or runtime cost.
 */
struct NoRollback
{
	static constexpr bool Enabled                = false; ///< Compile-time guard for rollback-conditional code.
	static constexpr uint32_t RollbackFrameCount = 0;     ///< No history frames allocated.

	void InitializeRings() {}

	template <typename TLogic>
	void ProcessRollback(TLogic&)
	{
	}
	template <typename TLogic>
	void ExecuteRollback(TLogic&, uint32_t)
	{
	}
	template <typename TLogic>
	void ExecuteRollbackTest(TLogic&)
	{
	}

	void RecordFrameInput(LogicThreadBase&) {}
	void InjectFrameInput(LogicThreadBase&, uint32_t) {}
	void SaveSnapshot(LogicThreadBase&) {}
	void ReplayServerEvents(LogicThreadBase&) {}
	void ApplyPredictedCorrections(LogicThreadBase&) {}

	void EnqueueCorrections(std::vector<EntityTransformCorrection>, uint32_t) {}
	void EnqueuePredictedCorrections(std::vector<EntityTransformCorrection>) {}
	void EnqueueSpawnRollback(LogicThreadBase&, uint32_t) {}
};

/**
 * @brief TRollbackMode policy for `LogicThread` — owns all rollback state and logic.
 *
 * Template method bodies live in `Core/Private/RollbackImpl.h` (included only by `LogicThread.cpp`, which instantiates them); non-template bodies in `RollbackSim.cpp`.
 *
 * @note `IncomingCorrections` and `IncomingPredictedCorrections` are the only members
 *       safe to write from net/worker threads. All other members are Logic-thread-only.
 * @warning `EnqueueCorrections` and `EnqueuePredictedCorrections` may be called from any
 *          thread; all other methods must be called on the Logic thread.
 */
struct RollbackSim
{
	static constexpr bool Enabled                = true; ///< Enables rollback paths in LogicThread.
	static constexpr uint32_t RollbackFrameCount = 5;    ///< Frames of history kept for resimulation.

	TrinyxMPMCRing<EntityTransformCorrection> IncomingCorrections;          ///< Lock-free; net/worker threads push, Logic thread drains.
	TrinyxMPMCRing<EntityTransformCorrection> IncomingPredictedCorrections; ///< Lock-free; net/worker threads push, Logic thread drains.

	std::vector<EntityTransformCorrection> PendingCorrections;         ///< Logic-thread staging; drained from IncomingCorrections each tick.
	std::queue<EntityTransformCorrection> PendingPredictedCorrections; ///< Logic-thread staging; drained from IncomingPredictedCorrections each tick.

#ifdef TNX_TESTING
	std::vector<uint8_t> TemporalSlabBackup; ///< Pre-rollback slab snapshot for determinism validation.
	std::vector<uint8_t> VolatileSlabBackup; ///< Pre-rollback slab snapshot for determinism validation.

	/// @brief Snapshot of the ground-truth ECS frame captured before rollback resimulation.
	///
	/// Stores field bytes plus the entity counts from @c TemporalFrameHeader and the
	/// per-field @c CurrentUsed values so the comparator can detect entity-count drift
	/// independently of the raw data check (e.g. correct resim values but fewer entities).
	struct GroundTruthSnapshot
	{
		std::vector<uint8_t> Data;  ///< Raw field bytes (one frame, header excluded).
		uint32_t ActiveEntityCount; ///< TemporalFrameHeader::ActiveEntityCount at capture.
		uint32_t TotalAllocated;    ///< TemporalFrameHeader::TotalAllocatedEntities at capture.
		std::vector<size_t> FieldUsed;
		///< FieldCompareInfo::CurrentUsed per field at capture,
		///< parallel to GetValidFieldInfos() order.
	};

	GroundTruthSnapshot GroundTruth; ///< Populated by ExecuteRollbackTest before calling ExecuteRollback.
#endif

	void InitializeRings();

	/**
	 * @brief Drains incoming rings and triggers resimulation if corrections are pending.
	 * @tparam TLogic Concrete LogicThread specialization.
	 */
	template <typename TLogic>
	void ProcessRollback(TLogic& logic);

	/**
	 * @brief Restores Jolt + SoA state to `targetFrame` and resimulates forward.
	 * @tparam TLogic Concrete LogicThread specialization.
	 * @param targetFrame Earliest frame that received a correction; resim starts here.
	 * @return total number of frames resimulated.
	 */
	template <typename TLogic>
	uint32_t ExecuteRollback(TLogic& logic, uint32_t targetFrame);

	/**
	 * @brief Determinism self-test: resimulates the last N frames and diffs against saved ground truth.
	 * @tparam TLogic Concrete LogicThread specialization.
	 * @note Only meaningful with `TNX_TESTING` defined.
	 */
	template <typename TLogic>
	void ExecuteRollbackTest(TLogic& logic);

	/** @brief Saves this frame's input into the per-frame input log for future resimulation. */
	void RecordFrameInput(LogicThreadBase& logic);

	/**
	 * @brief Replays a previously recorded input into the sim during resimulation.
	 * @param frameNum The historical frame whose input is being replayed.
	 */
	void InjectFrameInput(LogicThreadBase& logic, uint32_t frameNum);

	/** @brief Saves a Jolt + SoA snapshot for the current frame into the ring buffer. */
	void SaveSnapshot(LogicThreadBase& logic);

	/** @brief Re-applies Authority-originated events (spawns, despawns) during resimulation. */
	void ReplayServerEvents(LogicThreadBase& logic);

	/** @brief Applies speculative corrections accumulated in `PendingPredictedCorrections`. */
	void ApplyPredictedCorrections(LogicThreadBase& logic);

	/**
	 * @brief Enqueues Authority-confirmed transform corrections; safe to call from any thread.
	 * @param earliestClientFrame The oldest client frame affected; resim will start from here.
	 */
	void EnqueueCorrections(std::vector<EntityTransformCorrection> corrections, uint32_t earliestClientFrame);

	/**
	 * @brief Enqueues speculative (predicted) corrections; safe to call from any thread.
	 */
	void EnqueuePredictedCorrections(std::vector<EntityTransformCorrection> corrections);

	/**
	 * @brief Triggers a rollback to reconcile a spawn that arrived out-of-order.
	 * @param clientFrame The client frame at which the spawn should have occurred.
	 */
	void EnqueueSpawnRollback(LogicThreadBase& logic, uint32_t clientFrame);
};

/** @} */
