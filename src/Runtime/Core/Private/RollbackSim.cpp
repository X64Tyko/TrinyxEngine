#ifdef TNX_ENABLE_ROLLBACK

#include "Policies/RollbackPolicy.h"
#include "LogicThreadBase.h"
#include "TemporalComponentCache.h"
#include "JoltPhysics.h"
#include "Registry.h"
#include "Input.h"
#include "Logger.h"

void RollbackSim::InitializeRings()
{
    IncomingCorrections.Initialize(256);
    IncomingPredictedCorrections.Initialize(256);
}

void RollbackSim::RecordFrameInput(LogicThreadBase& logic)
{
    TemporalFrameHeader* header = logic.TemporalCache->GetFrameHeader();
    std::memcpy(header->InputKeyState, logic.SimInput->KeyState[logic.SimInput->ReadSlot], 64);
    header->InputMouseDX = logic.SimInput->GetMouseDX();
    header->InputMouseDY = logic.SimInput->GetMouseDY();
}

void RollbackSim::InjectFrameInput(LogicThreadBase& logic, uint32_t frameNum)
{
    TemporalFrameHeader* header = logic.TemporalCache->GetFrameHeader(frameNum);
    uint8_t readSlot            = logic.SimInput->ReadSlot;
    std::memcpy(logic.SimInput->KeyState[readSlot], header->InputKeyState, 64);
    logic.SimInput->MouseDX[readSlot]    = header->InputMouseDX;
    logic.SimInput->MouseDY[readSlot]    = header->InputMouseDY;
    logic.SimInput->EventCount[readSlot] = 0;
    logic.SimInput->ReadCursor           = 0;
}

void RollbackSim::SaveSnapshot(LogicThreadBase& logic)
{
    logic.PhysicsPtr->SaveSnapshot(logic.FrameNumber);
}

void RollbackSim::ReplayServerEvents(LogicThreadBase& logic)
{
    logic.RegistryPtr->ReplayServerEventsAt(logic.FrameNumber);
}

void RollbackSim::ApplyPredictedCorrections(LogicThreadBase& logic)
{
    EntityTransformCorrection corr{};
    while (IncomingPredictedCorrections.TryPop(corr))
    {
        if (corr.ClientFrame > logic.FrameNumber)
        {
            PendingPredictedCorrections.push(corr);
            continue;
        }
        logic.RegistryPtr->CheckAndCorrectEntityTransform(corr);
    }

    while (!PendingPredictedCorrections.empty())
    {
        corr = PendingPredictedCorrections.front();
        PendingPredictedCorrections.pop();
        if (!IncomingPredictedCorrections.TryPush(corr))
            LOG_ENG_WARN_F("[Rollback] Discarding stale predicted correction (frame %u)", corr.ClientFrame);
    }
}

void RollbackSim::EnqueueCorrections(std::vector<EntityTransformCorrection> corrections,
                                     uint32_t /*earliestClientFrame*/)
{
    for (auto& c : corrections)
    {
        if (!IncomingCorrections.TryPush(std::move(c)))
            LOG_ENG_WARN("[Rollback] IncomingCorrections full — correction dropped");
    }
}

void RollbackSim::EnqueuePredictedCorrections(std::vector<EntityTransformCorrection> corrections)
{
    for (auto& c : corrections)
    {
        if (!IncomingPredictedCorrections.TryPush(std::move(c)))
            LOG_ENG_WARN("[Rollback] IncomingPredictedCorrections full — correction dropped");
    }
}

void RollbackSim::EnqueueSpawnRollback(LogicThreadBase& logic, uint32_t clientFrame)
{
	// Warn and skip — ring can't reach that far back.
	if (logic.TemporalCache && logic.FrameNumber > logic.TemporalCache->GetTotalFrameCount())
	{
		const uint32_t oldestFrame = logic.FrameNumber - logic.TemporalCache->GetTotalFrameCount();
		if (clientFrame < oldestFrame)
		{
			LOG_WARN_F("[RollbackSim] EnqueueSpawnRollback: target frame %u is outside ring buffer (oldest=%u) — spawn replay skipped",
					   clientFrame, oldestFrame);
			return;
		}
	}

	uint32_t current = logic.PendingRollbackFrame.load(std::memory_order_relaxed);
    while (clientFrame < current)
    {
        if (logic.PendingRollbackFrame.compare_exchange_weak(current, clientFrame,
                                                              std::memory_order_release,
                                                              std::memory_order_relaxed))
            break;
    }
}

#endif // TNX_ENABLE_ROLLBACK
