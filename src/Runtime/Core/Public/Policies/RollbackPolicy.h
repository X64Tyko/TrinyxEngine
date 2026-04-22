#pragma once
#include <cstdint>
#include <queue>
#include <vector>

#include "NetTypes.h"
#include "TrinyxMPMCRing.h"

class LogicThreadBase;

// ---------------------------------------------------------------------------
// NoRollback — zero-size, zero overhead.
// All stubs are empty so LogicThread call sites need no if constexpr guards.
// ---------------------------------------------------------------------------
struct NoRollback
{
    static constexpr bool     Enabled            = false;
    static constexpr uint32_t RollbackFrameCount = 0;

    void InitializeRings() {}

    template <typename TLogic> void ProcessRollback(TLogic&) {}
    template <typename TLogic> void ExecuteRollback(TLogic&, uint32_t) {}
    template <typename TLogic> void ExecuteRollbackTest(TLogic&) {}

    void RecordFrameInput(LogicThreadBase&) {}
    void InjectFrameInput(LogicThreadBase&, uint32_t) {}
    void SaveSnapshot(LogicThreadBase&) {}
    void ReplayServerEvents(LogicThreadBase&) {}
    void ApplyPredictedCorrections(LogicThreadBase&) {}

    void EnqueueCorrections(std::vector<EntityTransformCorrection>, uint32_t) {}
    void EnqueuePredictedCorrections(std::vector<EntityTransformCorrection>) {}
    void EnqueueSpawnRollback(LogicThreadBase&, uint32_t) {}
};

// ---------------------------------------------------------------------------
// RollbackSim — owns all rollback state and logic.
// Method bodies live in RollbackImpl.h (templates) and RollbackSim.cpp (non-templates).
// ---------------------------------------------------------------------------
struct RollbackSim
{
    static constexpr bool     Enabled            = true;
    static constexpr uint32_t RollbackFrameCount = 5;

    // Lock-free rings: net/worker threads push here; logic thread drains.
    TrinyxMPMCRing<EntityTransformCorrection> IncomingCorrections;
    TrinyxMPMCRing<EntityTransformCorrection> IncomingPredictedCorrections;

    // Logic-thread-only staging (drained from IncomingCorrections each tick).
    std::vector<EntityTransformCorrection> PendingCorrections;
    std::queue<EntityTransformCorrection>  PendingPredictedCorrections;

#ifdef TNX_TESTING
    std::vector<uint8_t> TemporalSlabBackup;
    std::vector<uint8_t> VolatileSlabBackup;
    std::vector<uint8_t> GroundTruthBackup;
#endif

    void InitializeRings();

    template <typename TLogic> void ProcessRollback(TLogic& logic);
    template <typename TLogic> void ExecuteRollback(TLogic& logic, uint32_t targetFrame);
    template <typename TLogic> void ExecuteRollbackTest(TLogic& logic);

    void RecordFrameInput(LogicThreadBase& logic);
    void InjectFrameInput(LogicThreadBase& logic, uint32_t frameNum);
    void SaveSnapshot(LogicThreadBase& logic);
    void ReplayServerEvents(LogicThreadBase& logic);
    void ApplyPredictedCorrections(LogicThreadBase& logic);

    void EnqueueCorrections(std::vector<EntityTransformCorrection> corrections, uint32_t earliestClientFrame);
    void EnqueuePredictedCorrections(std::vector<EntityTransformCorrection> corrections);
    void EnqueueSpawnRollback(LogicThreadBase& logic, uint32_t clientFrame);
};
