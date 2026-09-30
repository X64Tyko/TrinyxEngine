#pragma once

#include "FlowManagerBase.h"
#include "StreamingManager.h"
#include "World.h"
#include "EntityBuilder.h"
#include "LogicThreadBase.h"
#include "Logger.h"
#ifdef TNX_ENABLE_NETWORK
#include "OwnerNet.h"
#include "OwnerSim.h"
#endif
#ifdef TNX_ENABLE_EDITOR
#include "PIENetThread.h"
#endif

/** @addtogroup flow
 *  @{
 */

// ---------------------------------------------------------------------------
// FlowManager<TNet, TRollback, TFrame> — Concrete typed flow manager.
//
// Small header-only template class derived from FlowManagerBase.
// The ONLY things here are:
//   - CreateWorldImpl(): creates World<TNet,TRollback,TFrame>
//   - LoadLevel(const char*, bool): replaces #ifdef with if constexpr
//   - GetTypedWorld(): typed accessor
//
// All data and public API live in FlowManagerBase.
// ---------------------------------------------------------------------------
template <typename TNet, typename TRollback, typename TFrame>
class FlowManager : public FlowManagerBase
{
public:
	using WorldT = World<TNet, TRollback, TFrame>;

	WorldT* GetTypedWorld() const { return TypedWorld; }

protected:
	WorldBase* CreateWorldImpl() override;
	void LoadLevel(const char* levelPath, bool bBackground = false) override;

private:
	WorldT* TypedWorld = nullptr; // non-owning alias; FlowManagerBase::ActiveWorld owns
};

// ---------------------------------------------------------------------------
// Template method bodies
// ---------------------------------------------------------------------------

template <typename TNet, typename TRollback, typename TFrame>
WorldBase* FlowManager<TNet, TRollback, TFrame>::CreateWorldImpl()
{
	auto typed  = std::make_unique<WorldT>();
	TypedWorld  = typed.get();
	ActiveWorld = std::move(typed);

	if (!TypedWorld->Initialize(*Config, &ConstructReg, WindowWidth, WindowHeight))
	{
		LOG_ENG_ERROR("[FlowManager] World::Initialize failed");
		ActiveWorld.reset();
		TypedWorld = nullptr;
		return nullptr;
	}

	ActiveWorld->SetFlowManager(this);
	LOG_ENG_INFO("[FlowManager] World created");
	return ActiveWorld.get();
}

template <typename TNet, typename TRollback, typename TFrame>
void FlowManager<TNet, TRollback, TFrame>::LoadLevel(const char* levelPath, bool bBackground)
{
	if (!ActiveWorld)
	{
		LOG_ENG_ERROR("[FlowManager] LoadLevel called with no active World");
		return;
	}

	if (!levelPath || levelPath[0] == '\0')
	{
		LOG_ENG_ERROR("[FlowManager] LoadLevel called with empty path");
		return;
	}

	ActiveLevelPath = levelPath;

	StreamingManager* sm = GetStreamingManager();
	if (!sm)
	{
		LOG_ENG_ERROR("[FlowManager] LoadLevel: StreamingManager not available");
		return;
	}

	// Build completion callback — OwnerSim worlds bind AcknowledgeLevelReady on the right handler.
	StreamingOnComplete onComplete;
#if defined(TNX_ENABLE_NETWORK) && !defined(TNX_NET_MODEL_SERVER)
	if constexpr (std::is_same_v<TNet, OwnerSim>)
	{
		using NetT = typename TNet::NetThreadType;
		auto* net  = static_cast<NetT*>(GetRawNetThread());
		if (net) onComplete.Bind<NetT, &NetT::AcknowledgeLevelReady>(net);
	}
#endif

	// Pack ownerID into the request for PIE routing; solo/server worlds return 0.
	const StreamingRequestID id = sm->BeginRequest(onComplete, ActiveWorld->GetLocalOwnerID());
	if (id == InvalidStreamingRequest) return;

	const char* pathPtr = sm->CopyPath(id, levelPath);
	Registry* reg       = ActiveWorld->GetRegistry();
	WorldBase* world    = ActiveWorld.get();
	Soul* soul          = GetSoul(ActiveWorld->GetLocalOwnerID());

	if constexpr (TRollback::Enabled)
	{
		const uint32_t spawnFrame = ActiveWorld->GetLogicThread()->GetLastCompletedFrame() + 1;
		// Clamp rollback horizon before the job runs so no snapshot taken before this frame
		// can be restored (level geometry wouldn't exist in those snapshots).
		ActiveWorld->GetLogicThread()->SetEarliestValidRollbackFrame(spawnFrame);

		sm->AddJob(id, [world, reg, pathPtr, bBackground, spawnFrame, soul](uint32_t)
		{
			// Pre-warm all named asset refs so entities spawn with assets already loaded.
			FlowManagerBase::PreloadLevelAssets(pathPtr);

			// Blocks this worker until the Logic thread processes the spawn.
			// Safe: Logic thread drains its WorldQueue independently of General workers.
			world->SpawnAndWait([reg, pathPtr, bBackground, spawnFrame, soul](uint32_t)
			{
				std::vector<GlobalEntityHandle> handles;
				const size_t count = EntityBuilder::SpawnFromFileTracked(reg, pathPtr, bBackground, handles);
				LOG_NET_INFO_F(soul, "[FlowManager] LoadLevel: spawned %zu entities from %s%s at frame %u",
					count, pathPtr, bBackground ? " (Alive-only)" : "", spawnFrame);
#ifdef TNX_ENABLE_ROLLBACK
				for (GlobalEntityHandle gh : handles)
					reg->PushEntityReinitEvent(gh, spawnFrame);
#endif
			});
		});
	}
	else
	{
		sm->AddJob(id, [world, reg, pathPtr, bBackground, soul](uint32_t)
		{
			// Pre-warm all named asset refs so entities spawn with assets already loaded.
			FlowManagerBase::PreloadLevelAssets(pathPtr);

			world->SpawnAndWait([reg, pathPtr, bBackground, soul](uint32_t)
			{
				const size_t count = EntityBuilder::SpawnFromFile(reg, pathPtr, bBackground);
				LOG_NET_INFO_F(soul, "[FlowManager] LoadLevel: spawned %zu entities from %s%s",
					count, pathPtr, bBackground ? " (Alive-only)" : "");
			});
		});
	}

	sm->SubmitRequest(id);
}

/** @} */
