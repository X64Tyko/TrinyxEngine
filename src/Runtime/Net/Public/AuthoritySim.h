#pragma once
#include <cstdint>

#include "EngineConfig.h"
#include "FlowManager.h"
#include "Input.h"
#include "Logger.h"
#include "NetConnectionManager.h"
#include "NetTypes.h"
#include "PlayerInputLog.h"
#include "ReplicationSystem.h"
#include "WorldBase.h"

// ---------------------------------------------------------------------------
// AuthoritySim — server-side net policy for LogicThread<AuthoritySim, ...>.
//
// Template methods are inline (bodies below the class) so they are visible at
// the point of explicit instantiation in LogicThread.cpp.
//
// Initialized by AuthorityNet::WireNetMode(WorldBase*) after the world is created.
// Holds individual component pointers (not an AuthorityNet* back-pointer).
// ---------------------------------------------------------------------------

struct AuthoritySim
{
	// Called after SimInput->Swap(). Returns true to stall the sim tick
	// (waiting for a player's input window to arrive).
	// Only the 3 logic-dependent values are extracted here; everything else
	// lives in RunSimInput().
	template <typename TLogic>
	bool OnSimInput(uint32_t frameNumber, TLogic& logic)
	{
		uint32_t rollbackFrame = UINT32_MAX;
		const bool stall = RunSimInput(frameNumber,
		                               logic.IsResimulating(),
		                               logic.GetLastCompletedFrame(),
		                               rollbackFrame);
		if (rollbackFrame != UINT32_MAX)
			logic.RequestRollback(rollbackFrame);
		return stall;
	}

	// Called after PublishCompletedFrame, before PropagateFrame.
	template <typename TLogic>
	void OnFramePublished(uint32_t frameNumber, TLogic& logic);

	// Called by AuthorityNet::WireNetMode(WorldBase*) once after world init.
	void Initialize(ReplicationSystem* replicator, NetConnectionManager* connMgr,
					const EngineConfig* config, WorldBase* world);

private:
	// Non-template core: all logic that doesn't need TLogic.
	// outRollbackFrame is set to the frame that should be passed to
	// logic.RequestRollback(); UINT32_MAX means no rollback this tick.
	bool RunSimInput(uint32_t frameNumber, bool isResimulating,
	                 uint32_t lastCompleted, uint32_t& outRollbackFrame);

	// Helper: look up the PlayerInputLog for ownerID via the replication channel.
	PlayerInputLog* GetInputLog(uint8_t ownerID)
	{
		if (!Replicator) return nullptr;
		ServerClientChannel* ch = Replicator->GetChannelIfActive(ownerID);
		return ch ? &ch->InputLog : nullptr;
	}

	ReplicationSystem* Replicator = nullptr;
	NetConnectionManager* ConnMgr = nullptr;
	const EngineConfig* Config    = nullptr;
	WorldBase* NetWorld               = nullptr;

	// Coalesced rollback target: the earliest input-mismatch frame seen during
	// Pass 2 injection this tick. Fired as one RequestRollback at the top of
	// the next non-resim OnSimInput call instead of calling RequestRollback
	// inside the injection loop (which would trigger recursive rollbacks).
	uint32_t PendingInputResimFrame = UINT32_MAX;
};


template <typename TLogic>
void AuthoritySim::OnFramePublished(uint32_t frameNumber, TLogic& /*logic*/)
{
	// Advance the committed frame horizon so Step 7 (networked despawn) can gate
	// phase-1 graduation: a dead entity's net slot is not freed until all players
	// have inputs confirmed past the death frame.
	if (Replicator) Replicator->AdvanceCommittedHorizon(frameNumber);
}
