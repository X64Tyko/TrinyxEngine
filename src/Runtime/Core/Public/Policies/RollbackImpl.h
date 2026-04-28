#pragma once
// Template method bodies for RollbackSim.
// Included at the bottom of LogicThread.h after all types are defined.
// Only compiled when TNX_ENABLE_ROLLBACK is defined.
#ifdef TNX_ENABLE_ROLLBACK

#include "TemporalComponentCache.h"
#include "JoltPhysics.h"
#include "Registry.h"
#include "Logger.h"
#include "Profiler.h"

template <typename TLogic>
void RollbackSim::ProcessRollback(TLogic& logic)
{
    // Drop server events that have aged past the temporal ring.
    if (logic.FrameNumber > logic.TemporalCache->GetTotalFrameCount())
        logic.RegistryPtr->PruneServerEvents(logic.FrameNumber - logic.TemporalCache->GetTotalFrameCount());

    if (logic.bRollbackTestRequested.load(std::memory_order_acquire)
        && logic.FrameNumber > RollbackFrameCount + logic.PhysicsDivizor)
    {
        logic.bRollbackTestRequested.store(false, std::memory_order_relaxed);
        ExecuteRollbackTest(logic);
    }

    if (logic.bRollbackActive) return;

    // Drain any corrections that arrived from worker/net threads since last tick.
    {
        EntityTransformCorrection staged;
        while (IncomingCorrections.TryPop(staged)) PendingCorrections.push_back(staged);
    }

    // Merge any pending spawn rollback request.
    uint32_t spawnRollbackFrame = logic.PendingRollbackFrame.exchange(UINT32_MAX, std::memory_order_acq_rel);

    // Drop corrections whose target frame predates the temporal ring.
    {
        const uint32_t ringSize   = logic.TemporalCache->GetTotalFrameCount();
        const uint32_t currentF   = logic.TemporalCache->GetFrameHeader()->FrameNumber;
        const uint32_t oldestSlab = (currentF >= ringSize - 1) ? (currentF - (ringSize - 1)) : 0u;

        const auto stalePred = [oldestSlab](const EntityTransformCorrection& c)
        {
            return c.ClientFrame < oldestSlab;
        };

        auto staleBegin = std::remove_if(PendingCorrections.begin(), PendingCorrections.end(), stalePred);
        if (staleBegin != PendingCorrections.end())
        {
            LOG_ENG_WARN_F("[Rollback] Discarding %zu stale correction(s) (frame < %u, ring depth=%u)",
                           std::distance(staleBegin, PendingCorrections.end()), oldestSlab, ringSize);
            PendingCorrections.erase(staleBegin, PendingCorrections.end());
        }
    }

    uint32_t minFrame = spawnRollbackFrame;
    if (!PendingCorrections.empty())
        for (const auto& c : PendingCorrections) if (c.ClientFrame < minFrame) minFrame = c.ClientFrame;

    if (minFrame != UINT32_MAX)
    {
        if (logic.PhysicsPtr)
        {
            const uint32_t oldestSnap = logic.PhysicsPtr->GetOldestSnapshotFrame();
            if (oldestSnap != UINT32_MAX && minFrame < oldestSnap)
            {
                LOG_ENG_WARN_F("[Rollback] Clamping target frame %u → %u (oldest Jolt snapshot)",
                               minFrame, oldestSnap);
                minFrame = oldestSnap;
            }
        }
        ExecuteRollback(logic, minFrame);
    }
}

template <typename TLogic>
void RollbackSim::ExecuteRollback(TLogic& logic, uint32_t targetFrame)
{
    TNX_ZONE_N("Rollback");

    logic.bRollbackActive = true;

    const uint32_t T           = logic.FrameNumber - 1;
    const uint32_t frameCount  = logic.TemporalCache->GetTotalFrameCount();
    const double fixedStepTime = logic.ConfigPtr->GetFixedStepTime();

    const uint32_t alignedTarget = (targetFrame / logic.PhysicsDivizor) * logic.PhysicsDivizor - 1;

    if (alignedTarget >= T)
    {
        LOG_ENG_WARN_F("[Rollback] Target frame %u (aligned from %u) is at or beyond current frame %u — skipping",
                       alignedTarget, targetFrame, T);
        logic.bRollbackActive = false;
        PendingCorrections.clear();
        return;
    }

    const uint32_t totalResimFrames = (T - alignedTarget) + 1;

    LOG_ENG_INFO_F("[Rollback] Rewind to frame %u (aligned from %u), resim %u frames to frame %u",
                   alignedTarget, targetFrame, totalResimFrames, T);

    // ── Rewind ──────────────────────────────────────────────────────────────
    {
        TNX_ZONE_N("Rollback_Rewind");

        logic.TemporalCache->SetActiveWriteFrame(alignedTarget % frameCount);

        TrinyxJobs::WaitForCounter(logic.PhysicsPtr->GetJoltPhysCounter(), TrinyxJobs::Queue::Logic);

        if (!logic.PhysicsPtr->RestoreSnapshot(alignedTarget))
        {
            LOG_ENG_WARN("[Rollback] Snapshot not found, falling back to rebuild-from-slab");
            logic.PhysicsPtr->ResetAllBodies();
            logic.PhysicsPtr->FlushPendingBodies(logic.RegistryPtr);
        }

        logic.FrameNumber = alignedTarget;
        logic.RegistryPtr->ReplayServerEventsAt(logic.FrameNumber);
        logic.PhysicsPtr->SaveSnapshot(alignedTarget);
        logic.RegistryPtr->PropagateFrame(logic.FrameNumber++);

        logic.SimulationTime = logic.FrameNumber * fixedStepTime;
    }

    LOG_ENG_INFO_F("[Rollback] Jolt restored, starting resim from frame %u", logic.FrameNumber);

    // ── Resimulate ──────────────────────────────────────────────────────────
    {
        TNX_ZONE_N("Rollback_Resim");

        for (uint32_t i = 0; i < totalResimFrames; ++i)
        {
            InjectFrameInput(logic, logic.FrameNumber);
            logic.NetMode.OnSimInput(logic.FrameNumber, logic);
			logic.PhysicsLoop(SimFloat(fixedStepTime));

			for (auto it = PendingCorrections.begin(); it != PendingCorrections.end();)
            {
                if (it->ClientFrame != logic.FrameNumber) { ++it; continue; }
                if (logic.RegistryPtr->CheckAndCorrectEntityTransform(*it))
                {
                    LOG_ENG_INFO_F("[Rollback] Correction applied at frame %u for netHandle=%u",
                                   logic.FrameNumber, it->NetHandle);
                }
                it = PendingCorrections.erase(it);
            }

            logic.RegistryPtr->ReplayServerEventsAt(logic.FrameNumber);
            logic.RegistryPtr->PropagateFrame(logic.FrameNumber++);
        }

        LOG_ENG_INFO_F("[Rollback] Resimulation complete, frame %u", logic.FrameNumber);
    }

    PendingCorrections.erase(
        std::remove_if(PendingCorrections.begin(), PendingCorrections.end(),
                       [&logic](const EntityTransformCorrection& c) { return c.ClientFrame < logic.FrameNumber; }),
        PendingCorrections.end());
    logic.bRollbackActive = false;
}

template <typename TLogic>
void RollbackSim::ExecuteRollbackTest(TLogic& logic)
{
    TNX_ZONE_N("Rollback_Test");

    const uint32_t T             = logic.FrameNumber - 1;
    const uint32_t rollbackTarget = T - RollbackFrameCount;
    [[maybe_unused]] const double fixedStepTime = logic.ConfigPtr->GetFixedStepTime();

#ifdef TNX_TESTING
    const size_t fieldDataSize             = logic.TemporalCache->GetFrameStride() - sizeof(TemporalFrameHeader);
    const uint32_t groundTruthSlot         = logic.TemporalCache->GetActiveReadFrame();
    TemporalFrameHeader* groundTruthHeader = logic.TemporalCache->GetFrameHeader(groundTruthSlot);
    uint8_t* groundTruthFieldData          = reinterpret_cast<uint8_t*>(groundTruthHeader) + sizeof(TemporalFrameHeader);

    ComponentCacheBase* volatileCache = logic.RegistryPtr->GetVolatileCache();
    const size_t temporalSlabSize     = logic.TemporalCache->GetTotalSlabSize();
    const size_t volatileSlabSize     = volatileCache->GetTotalSlabSize();

    {
        TNX_ZONE_N("Rollback_Backup");

        GroundTruthBackup.resize(fieldDataSize);
        std::memcpy(GroundTruthBackup.data(), groundTruthFieldData, fieldDataSize);

        TemporalSlabBackup.resize(temporalSlabSize);
        VolatileSlabBackup.resize(volatileSlabSize);
        std::memcpy(TemporalSlabBackup.data(), logic.TemporalCache->GetSlabPtr(), temporalSlabSize);
        std::memcpy(VolatileSlabBackup.data(), volatileCache->GetSlabPtr(), volatileSlabSize);
    }

    const uint32_t savedTemporalWrite = logic.TemporalCache->GetActiveWriteFrame();
    const uint32_t savedTemporalRead  = logic.TemporalCache->GetActiveReadFrame();
    const uint32_t savedVolatileWrite = volatileCache->GetActiveWriteFrame();
    const uint32_t savedVolatileRead  = volatileCache->GetActiveReadFrame();

    JPH::StateRecorderImpl savedJolt;
    {
        TNX_ZONE_N("Rollback_SaveJolt");
        logic.PhysicsPtr->GetPhysicsSystem()->SaveState(savedJolt, JPH::EStateRecorderState::All);
    }

    const uint32_t savedFrameNumber = logic.FrameNumber;
    const double   savedSimTime     = logic.SimulationTime;
#endif

    ExecuteRollback(logic, rollbackTarget);

#ifdef TNX_TESTING
    {
        TNX_ZONE_N("Rollback_Compare");

        const uint32_t resimSlot         = logic.TemporalCache->GetActiveReadFrame();
        TemporalFrameHeader* resimHeader = logic.TemporalCache->GetFrameHeader(resimSlot);
        uint8_t* resimFieldData          = reinterpret_cast<uint8_t*>(resimHeader) + sizeof(TemporalFrameHeader);

        int cmp = std::memcmp(GroundTruthBackup.data(), resimFieldData, fieldDataSize);
        if (cmp == 0)
        {
            LOG_ENG_INFO_F("[Rollback] PASSED — byte-perfect determinism (%zu bytes, %u frames resimulated)",
                           fieldDataSize, RollbackFrameCount);
        }
        else
        {
            LOG_ENG_WARN_F("[Rollback] FAILED — divergence detected (%u frames resimulated)", RollbackFrameCount);

            auto fieldInfos = logic.TemporalCache->GetValidFieldInfos();
            for (const auto& info : fieldInfos)
            {
                if (info.CurrentUsed == 0) continue;
                const uint8_t* truthField = GroundTruthBackup.data() + info.OffsetInFrame;
                const uint8_t* resimField = resimFieldData + info.OffsetInFrame;

                int fieldCmp = std::memcmp(truthField, resimField, info.CurrentUsed);
                if (fieldCmp != 0)
                {
                    size_t firstDiff = 0;
                    for (size_t b = 0; b < info.CurrentUsed; ++b)
                        if (truthField[b] != resimField[b]) { firstDiff = b; break; }

                    size_t entityIdx   = firstDiff / info.FieldSize;
                    size_t byteInField = firstDiff % info.FieldSize;
                    size_t divergentBytes = 0;
                    for (size_t b = 0; b < info.CurrentUsed; ++b) divergentBytes += (truthField[b] != resimField[b]);

                    LOG_ENG_WARN_F("  DIVERGE: %s (comp=%u field=%zu) entity=%zu+%zu divergent=%zu/%zu (%.2f%%)",
                                   info.FieldName, info.CompType, info.FieldIndex,
                                   entityIdx, byteInField, divergentBytes, info.CurrentUsed,
                                   100.0 * static_cast<double>(divergentBytes) / static_cast<double>(info.CurrentUsed));
                }
            }
        }

        JPH::StateRecorderImpl resimJolt;
        logic.PhysicsPtr->GetPhysicsSystem()->SaveState(resimJolt, JPH::EStateRecorderState::All);
        std::string resimJoltData = resimJolt.GetData();
        std::string savedJoltData = savedJolt.GetData();

        if (resimJoltData == savedJoltData)
            LOG_ENG_INFO_F("[Rollback] Jolt physics: MATCH (%zu bytes)", resimJoltData.size());
        else
        {
            LOG_ENG_WARN_F("[Rollback] Jolt physics: DIVERGED (truth=%zu bytes, resim=%zu bytes)",
                           savedJoltData.size(), resimJoltData.size());
            size_t minLen = std::min(savedJoltData.size(), resimJoltData.size());
            for (size_t i = 0; i < minLen; ++i)
            {
                if (savedJoltData[i] != resimJoltData[i])
                {
                    LOG_ENG_WARN_F("  First Jolt divergence at byte %zu: truth=0x%02x resim=0x%02x",
                                   i, static_cast<uint8_t>(savedJoltData[i]), static_cast<uint8_t>(resimJoltData[i]));
                    break;
                }
            }
        }
    }

    {
        TNX_ZONE_N("Rollback_Restore");

        std::memcpy(logic.TemporalCache->GetSlabPtr(), TemporalSlabBackup.data(), temporalSlabSize);
        std::memcpy(volatileCache->GetSlabPtr(), VolatileSlabBackup.data(), volatileSlabSize);

        logic.TemporalCache->SetActiveWriteFrame(savedTemporalWrite);
        logic.TemporalCache->SetLastWrittenFrame(savedTemporalRead);
        volatileCache->SetActiveWriteFrame(savedVolatileWrite);
        volatileCache->SetLastWrittenFrame(savedVolatileRead);

        savedJolt.Rewind();
        logic.PhysicsPtr->GetPhysicsSystem()->RestoreState(savedJolt);

        logic.FrameNumber    = savedFrameNumber;
        logic.SimulationTime = savedSimTime;
    }
#endif // TNX_TESTING

    LOG_ENG_INFO("[Rollback] State restored, simulation continuing.");
}

#endif // TNX_ENABLE_ROLLBACK
