
#pragma once
#include "AssetRegistry.h"
#include "ConstructRegistry.h"
#include "GameMode.h"
#include "Logger.h"
#include "PlayerConstruct.h"
#include "ReflectionRegistry.h"
#include "ReplicationSystem.h"
#include "Soul.h"
#include "WithSpawnManagement.h"
#include "World.h"

// ---------------------------------------------------------------------------
// ArenaMode — Playground game mode.
//
// Handles player spawning: picks a spawn point, creates a PlayerConstruct,
// registers it for replication, and claims it on the Soul.
//
// Standalone path: OnPlayerJoined (via OnClientLoaded(0)) → SpawnPlayerBody
// Networked path:  OnPlayerBeginRequest (via PlayerBegin RPC) → SpawnPlayerBody (server)
//                  ReplicationSystem → ConstructSpawn → HandleConstructSpawn → ClaimBody (client)
// ---------------------------------------------------------------------------
class ArenaMode : public GameMode, public WithSpawnManagement
{
public:
	int64_t GetCharacterPrefab(const Soul& /*soul*/) const override
	{
		const AssetEntry* entry = AssetRegistry::Get().FindByName("DefaultCharacter");
		if (!entry)
		{
			LOG_WARN("[ArenaMode] DefaultCharacter asset not found in registry");
			return 0;
		}
		return entry->ID.GetUUID();
	}

	// Called by FlowManager when a client is fully loaded (LevelReady received).
	// In networked play the client will subsequently send a PlayerBeginRequest RPC
	// to trigger the actual spawn — don't spawn here. In Standalone there is no
	// RPC round-trip, so spawn immediately.
	void OnPlayerJoined(Soul& soul) override
	{
		if (GetWorld()->GetReplicationSystem()) return; // Networked: wait for PlayerBeginRequest RPC

		SpawnPlayerBody(soul);
	}

	PlayerBeginResult OnPlayerBeginRequest(Soul& soul, const PlayerBeginRequestPayload& req) override
	{
		if (!ValidateSpawn(soul, req))
		{
			PlayerBeginResult result;
			result.Accepted = false;
			return result;
		}

		SpawnPlayerBody(soul);

		PlayerBeginResult result;
		result.Accepted = true;
		result.PosX     = 0.0f;
		result.PosY     = 5.0f;
		result.PosZ     = 0.0f;
		result.Body     = soul.GetBodyHandle();
		return result;
	}

	const char* GetModeName() const override { return "ArenaMode"; }

private:
	void SpawnPlayerBody(Soul& soul)
	{
		World* world            = GetWorld();
		ConstructRegistry* reg  = world->GetConstructRegistry();
		ReplicationSystem* repl = world->GetReplicationSystem();

		const int64_t prefabIDRaw = GetCharacterPrefab(soul);
		const uint16_t typeHash   = ReflectionRegistry::ConstructTypeHashFromName("PlayerConstruct");

		auto* player         = reg->Create<PlayerConstruct>(world);
		player->PlayerSoul   = &soul;
		player->SpawnPosX    = 0.0f;
		player->SpawnPosY = 5.0f;
		player->SpawnPosZ = 0.0f;

		ConstructRef ref;
		if (repl)
		{
			ref = repl->RegisterConstruct<PlayerConstruct>(reg, player, soul.GetOwnerID(), typeHash, prefabIDRaw);
		}
		else
		{
			ConstructNetManifest manifest{};
			manifest.PrefabIndex = typeHash;
			manifest.NetFlags    = 0;
			ref                  = reg->AllocateNetRef(player, soul.GetOwnerID(), manifest, typeHash, prefabIDRaw);
		}

		soul.ClaimBody(ref);
		LOG_INFO_F("[ArenaMode] Spawned body for ownerID=%u (networked=%s)",
				   soul.GetOwnerID(), repl ? "yes" : "no");
	}
};
