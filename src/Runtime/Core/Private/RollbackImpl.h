#pragma once
/// @file RollbackImpl.h
/// @brief Template method bodies for @c RollbackSim.
///
/// Private to @c LogicThread.cpp: the only translation unit that instantiates these members
/// (via LogicThread's explicit instantiations). Only compiled when @c TNX_ENABLE_ROLLBACK is defined.
#ifdef TNX_ENABLE_ROLLBACK

#include "TemporalComponentCache.h"
#include "JoltPhysics.h"
#include "Registry.h"
#include "Logger.h"
#include "Profiler.h"

/** @addtogroup core
 *  @{
 */

/// @brief Called once per logic tick to drain corrections and trigger rollback if needed.
///
/// Prunes stale server events, drains @c IncomingCorrections, merges any pending
/// spawn-rollback frame, then calls @c ExecuteRollback if the earliest correction
/// or spawn event predates the current frame.
/// @param logic The concrete @c LogicThread instance that owns this policy.
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
		while (IncomingCorrections.TryPop(staged))
			PendingCorrections.push_back(staged);
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
		for (const auto& c : PendingCorrections)
			if (c.ClientFrame < minFrame) minFrame = c.ClientFrame;

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

/// @brief Rewind to @p targetFrame and resimulate forward to the current frame.
///
/// Restores the nearest Jolt snapshot at or before @p targetFrame, rewinds
/// the ECS slab write pointer, replays server events, then iterates the resim
/// loop — injecting stored input and applying @c EntityTransformCorrection entries
/// at the correct frame. Uses dirty-filtered scalar sweeps to skip unchanged entities.
///
/// @param targetFrame The simulation frame to rewind to.
template <typename TLogic>
uint32_t RollbackSim::ExecuteRollback(TLogic& logic, uint32_t targetFrame)
{
	TNX_ZONE_N("Rollback");

	logic.bRollbackActive = true;

	const uint32_t T           = logic.FrameNumber - 1;
	const uint32_t frameCount  = logic.TemporalCache->GetTotalFrameCount();
	const double fixedStepTime = logic.ConfigPtr->GetFixedStepTime();

	// physicsSnapFrame: the physics step boundary we can restore a Jolt snapshot from.
	// alignedTarget: the ECS slot one frame earlier — the frame whose slab we propagate forward
	// as the resim starting state, so the first resim step sees a clean pre-physics ECS frame.
	const uint32_t physicsSnapFrame = (targetFrame / logic.PhysicsDivizor) * logic.PhysicsDivizor;
	const uint32_t alignedTarget    = (physicsSnapFrame > 0) ? physicsSnapFrame - 1 : 0;

	if (alignedTarget >= T)
	{
		LOG_ENG_WARN_F("[Rollback] Target frame %u (aligned from %u) is at or beyond current frame %u — skipping",
			alignedTarget, targetFrame, T);
		logic.bRollbackActive = false;
		PendingCorrections.clear();
		return 0;
	}

	const uint32_t totalResimFrames = (T - alignedTarget) + 1;

	LOG_ENG_INFO_F("[Rollback] Rewind to frame %u (physics snap %u), resim %u frames to frame %u",
		alignedTarget, physicsSnapFrame, totalResimFrames, T);

	// ── Rewind ──────────────────────────────────────────────────────────────
	{
		TNX_ZONE_N("Rollback_Rewind");

		logic.TemporalCache->SetActiveWriteFrame(alignedTarget % frameCount);

		TrinyxJobs::WaitForCounter(logic.PhysicsPtr->GetJoltPhysCounter(), TrinyxJobs::Queue::Logic);

		if (!logic.PhysicsPtr->RestoreSnapshot(physicsSnapFrame))
		{
			LOG_ENG_WARN_F("[Rollback] Snapshot for physics frame %u not found, falling back to rebuild-from-slab",
				physicsSnapFrame);
			logic.PhysicsPtr->ResetAllBodies();
			logic.PhysicsPtr->FlushPendingBodies(logic.RegistryPtr);
		}

		logic.FrameNumber = alignedTarget;
		// Clear the old timeline's DirtiedFrame bits first: the resim propagates only entities
		// marked this timeline, so effects replayed here must set their bits after the clear.
		logic.RegistryPtr->ClearDirtiedFrameBits();
		logic.RegistryPtr->ApplyPendingServerEventsBefore(logic.FrameNumber);
		logic.RegistryPtr->ReplayServerEventsAt(logic.FrameNumber);
		logic.PhysicsPtr->SaveSnapshot(physicsSnapFrame);
		logic.RegistryPtr->PropagateFrame(logic.FrameNumber++, true);

		logic.SimulationTime = logic.FrameNumber * fixedStepTime;
	}

	LOG_ENG_INFO_F("[Rollback] Jolt restored, starting resim from frame %u", logic.FrameNumber);

	// ── Resimulate ──────────────────────────────────────────────────────────
	{
		TNX_ZONE_N("Rollback_Resim");

		// Redirect PrePhys/PostPhys sweeps to dirty-filtered scalar variants and
		// PropagateFrame to scatter-copy for the duration of the resim loop.
		logic.RegistryPtr->SetResimMode(true);

		for (uint32_t i = 0; i < totalResimFrames; ++i)
		{
			InjectFrameInput(logic, logic.FrameNumber);
			logic.NetMode.OnSimInput(logic.FrameNumber, logic);
			logic.PhysicsLoop(SimFloat(fixedStepTime));

			for (auto it = PendingCorrections.begin(); it != PendingCorrections.end();)
			{
				if (it->ClientFrame != logic.FrameNumber)
				{
					++it;
					continue;
				}
				if (logic.RegistryPtr->CheckAndCorrectEntityTransform(*it))
				{
					LOG_ENG_INFO_F("[Rollback] Correction applied at frame %u for netHandle=%u",
						logic.FrameNumber, it->NetHandle);
				}
				it = PendingCorrections.erase(it);
			}

			logic.RegistryPtr->ReplayServerEventsAt(logic.FrameNumber);
			logic.RegistryPtr->PropagateFrame(logic.FrameNumber++, true);
		}

		LOG_ENG_INFO_F("[Rollback] Resimulation complete, frame %u", logic.FrameNumber);

		logic.RegistryPtr->SetResimMode(false);
	}

	PendingCorrections.erase(
		std::remove_if(PendingCorrections.begin(), PendingCorrections.end(),
			[&logic](const EntityTransformCorrection& c)
	{
		return c.ClientFrame < logic.FrameNumber;
	}),
		PendingCorrections.end());
	logic.bRollbackActive = false;

	return totalResimFrames;
}

/// @brief Determinism test: roll back @c RollbackFrameCount frames and compare against ground truth.
///
/// Under @c TNX_TESTING, backs up the current slab and Jolt state, calls @c ExecuteRollback,
/// then does a byte-perfect @c memcmp of the resimulated field data against the saved ground
/// truth. Reports per-field divergences and Jolt state mismatches. Restores the original
/// simulation state before returning.
/// @param logic The concrete @c LogicThread instance that owns this policy.
template <typename TLogic>
void RollbackSim::ExecuteRollbackTest(TLogic& logic)
{
	TNX_ZONE_N("Rollback_Test");

	const uint32_t T                            = logic.FrameNumber - 1;
	const uint32_t rollbackTarget               = T - RollbackFrameCount;
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

		GroundTruth.Data.resize(fieldDataSize);
		std::memcpy(GroundTruth.Data.data(), groundTruthFieldData, fieldDataSize);

		GroundTruth.ActiveEntityCount = groundTruthHeader->ActiveEntityCount;
		GroundTruth.TotalAllocated    = groundTruthHeader->TotalAllocatedEntities;

		auto captureInfos = logic.TemporalCache->GetValidFieldInfos();
		GroundTruth.FieldUsed.resize(captureInfos.size());
		for (size_t i = 0; i < captureInfos.size(); ++i)
			GroundTruth.FieldUsed[i] = captureInfos[i].CurrentUsed;

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
	const double savedSimTime       = logic.SimulationTime;
#endif

	uint32_t totalResim = ExecuteRollback(logic, rollbackTarget);

#ifdef TNX_TESTING
	{
		TNX_ZONE_N("Rollback_Compare");

		const uint32_t resimSlot         = logic.TemporalCache->GetActiveReadFrame();
		TemporalFrameHeader* resimHeader = logic.TemporalCache->GetFrameHeader(resimSlot);
		uint8_t* resimFieldData          = reinterpret_cast<uint8_t*>(resimHeader) + sizeof(TemporalFrameHeader);

		// ActiveEntityCount/TotalAllocatedEntities in TemporalFrameHeader are written by
		// PublishCompletedFrame, which is not called during resimulation. Comparing header
		// entity counts here would always be a false positive — entity count drift is caught
		// by the Flags field comparison below (Active bit clear = tombstone = data divergence).
		bool anyFieldDiverged = false;

		// Field-by-field comparison — a raw memcmp would trigger on uninitialised
		// inter-field padding bytes which carry no simulation meaning.
		auto fieldInfos = logic.TemporalCache->GetValidFieldInfos();

		for (size_t fi = 0; fi < fieldInfos.size(); ++fi)
		{
			const auto& info = fieldInfos[fi];

			// CurrentUsed encodes allocated bytes (entityCount * fieldSize).
			// A mismatch here means entities were spawned or despawned during resim.
			const size_t truthUsed = fi < GroundTruth.FieldUsed.size() ? GroundTruth.FieldUsed[fi] : 0;
			if (truthUsed != info.CurrentUsed)
			{
				anyFieldDiverged = true;
				LOG_ENG_WARN_F("  DIVERGE (alloc): %s (comp=%u field=%zu) truth=%zu bytes resim=%zu bytes",
					info.FieldName, info.CompType, info.FieldIndex, truthUsed, info.CurrentUsed);
				continue;
			}

			if (info.CurrentUsed == 0) continue;

			const uint8_t* truthField = GroundTruth.Data.data() + info.OffsetInFrame;
			const uint8_t* resimField = resimFieldData + info.OffsetInFrame;

			int fieldCmp = std::memcmp(truthField, resimField, info.CurrentUsed);
			if (fieldCmp != 0)
			{
				anyFieldDiverged = true;

				size_t firstDiff = 0;
				for (size_t b = 0; b < info.CurrentUsed; ++b)
					if (truthField[b] != resimField[b])
					{
						firstDiff = b;
						break;
					}

				size_t entityIdx      = firstDiff / info.FieldSize;
				size_t byteInField    = firstDiff % info.FieldSize;
				size_t divergentBytes = 0;
				for (size_t b = 0; b < info.CurrentUsed; ++b)
					divergentBytes += (truthField[b] != resimField[b]);

				LOG_ENG_WARN_F("  DIVERGE: %s (comp=%u field=%zu) entity=%zu+%zu divergent=%zu/%zu (%.2f%%)",
					info.FieldName, info.CompType, info.FieldIndex,
					entityIdx, byteInField, divergentBytes, info.CurrentUsed,
					100.0 * static_cast<double>(divergentBytes) / static_cast<double>(info.CurrentUsed));
			}
		}

		if (!anyFieldDiverged)
			LOG_ENG_INFO_F("[Rollback] ECS fields: PASSED (%u (%u requested) frames resimulated)", totalResim,
				RollbackFrameCount);
		else
			LOG_ENG_WARN_F(
				"[Rollback] ECS fields: FAILED — field divergence detected (%u (%u requested) frames resimulated)", totalResim, RollbackFrameCount);

		JPH::StateRecorderImpl resimJolt;
		logic.PhysicsPtr->GetPhysicsSystem()->SaveState(resimJolt, JPH::EStateRecorderState::All);
		std::string resimJoltData = resimJolt.GetData();
		std::string savedJoltData = savedJolt.GetData();

		const bool joltMatch = (resimJoltData == savedJoltData);
		if (joltMatch)
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

		logic.bRollbackTestPassed.store(!anyFieldDiverged && joltMatch, std::memory_order_relaxed);
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

	logic.bRollbackTestComplete.store(true, std::memory_order_release);
#endif // TNX_TESTING

	LOG_ENG_INFO("[Rollback] State restored, simulation continuing.");
}

/** @} */
#endif // TNX_ENABLE_ROLLBACK
