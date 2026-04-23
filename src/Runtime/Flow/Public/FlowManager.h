#pragma once

#include "FlowManagerBase.h"
#include "World.h"
#include "EntityBuilder.h"
#include "LogicThreadBase.h"
#include "Logger.h"

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

	Registry* reg        = ActiveWorld->GetRegistry();
	const char* pathCStr = ActiveLevelPath.c_str();

	if constexpr (TRollback::Enabled)
	{
		const uint32_t spawnFrame = ActiveWorld->GetLogicThread()->GetLastCompletedFrame() + 1;
		// SpawnAndWait is synchronous — soul lifetime is not a concern here.
		// If this ever becomes async, the raw capture must be replaced.
		Soul* soul = GetSoul(ActiveWorld->GetLocalOwnerID());
		ActiveWorld->SpawnAndWait([reg, pathCStr, bBackground, spawnFrame, soul](uint32_t)
		{
			std::vector<GlobalEntityHandle> spawnedHandles;
			size_t count = EntityBuilder::SpawnFromFileTracked(reg, pathCStr, bBackground, spawnedHandles);
			LOG_NET_INFO_F(soul, "[FlowManager] LoadLevel: spawned %zu entities from %s%s at frame %u",
						   count, pathCStr, bBackground ? " (Alive-only)" : "", spawnFrame);
#ifdef TNX_ENABLE_ROLLBACK
			for (GlobalEntityHandle gh : spawnedHandles) reg->PushEntityReinitEvent(gh, spawnFrame);
#endif
		});
	}
	else
	{
		ActiveWorld->SpawnAndWait([reg, pathCStr, bBackground](uint32_t)
		{
			size_t count = EntityBuilder::SpawnFromFile(reg, pathCStr, bBackground);
			LOG_NET_INFO_F(nullptr, "[FlowManager] LoadLevel: spawned %zu entities from %s%s",
						   count, pathCStr, bBackground ? " (Alive-only)" : "");
		});
	}

	LOG_NET_INFO_F(nullptr, "[FlowManager] Level loaded: %s", levelPath);
}
