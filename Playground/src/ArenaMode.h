
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
#include "TriggerVolume.h"
#include "CubeEntity.h"

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
	void Initialize(WorldBase* world) override
	{
		GameMode::Initialize(world);

		world->SpawnAndWait([this, world](uint32_t)
		{
			Registry* reg         = world->GetRegistry();
			ConstructRegistry* cr = world->GetConstructRegistry();

			// Spawn the target cube at (5, 2, 0).
			TargetCube = reg->Create<CubeEntity<>>([](CubeEntity<>& v)
			{
				v.SetFlags(TemporalFlagBits::Active | TemporalFlagBits::Alive | TemporalFlagBits::Replicated);
				v.transform.PosX = SimFloat(5.0f);
				v.transform.PosY = SimFloat(2.0f);
				v.transform.PosZ = SimFloat(-6.0f);
				v.transform.Rotation.SetIdentity();
				v.scale.ScaleX         = SimFloat(1.0f);
				v.scale.ScaleY         = SimFloat(1.0f);
				v.scale.ScaleZ         = SimFloat(1.0f);
				v.color.R              = SimFloat(1.0f);
				v.color.G              = SimFloat(0.3f);
				v.color.B              = SimFloat(0.1f);
				v.color.A              = SimFloat(1.0f);
				v.mesh.MeshID          = 1u;
				v.physBody.Shape       = JoltShapeType::Box;
				v.physBody.HalfExtentX = SimFloat(0.5f);
				v.physBody.HalfExtentY = SimFloat(0.5f);
				v.physBody.HalfExtentZ = SimFloat(0.5f);
				v.physBody.Motion      = JoltMotion::Dynamic;
				v.physBody.Mass        = SimFloat(10.0f);
			});

			// Spawn the trigger volume across from the cube and off the ground
			cr->Create<TriggerVolume>(world, [this](TriggerVolume* tv)
			{
				tv->PosX  = SimFloat(-5.0f);
				tv->PosY  = SimFloat(3.0f);
				tv->PosZ  = SimFloat(-6.0f);
				tv->HalfX = SimFloat(1.5f);
				tv->HalfY = SimFloat(1.5f);
				tv->HalfZ = SimFloat(1.5f);

				tv->OnEnter.Bind<ArenaMode, &ArenaMode::OnTriggerOverlap>(this);
			});
		});
	}

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
	// In networked play, remote clients send a PlayerBeginRequest RPC — don't spawn for them here.
	// In Standalone and Host, the local player (ownerID=0) spawns immediately.
	void OnPlayerJoined(Soul& soul) override
	{
		if (GetWorld()->GetReplicationSystem() && soul.GetOwnerID() != 0) return;

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
		result.PosX     = SimFloat(0.0f);
		result.PosY     = SimFloat(5.0f);
		result.PosZ     = SimFloat(0.0f);
		result.Body     = soul.GetBodyHandle();
		return result;
	}

private:
	int SpawnCounter = 0;
	EntityHandle TargetCube{};

	void OnTriggerOverlap(PhysicsOverlapData /*data*/)
	{
		if (!TargetCube.IsValid()) return;
		WorldBase* world = GetWorld();
		if (!world) return;

		world->GetRegistry()->Destroy(TargetCube);
		TargetCube = {};

		world->TriggerAudio(TNX_NAME("trigger_hit"));
		LOG_INFO("[ArenaMode] Trigger fired — cube destroyed");
	}

	void SpawnPlayerBody(Soul& soul)
	{
		WorldBase* world            = GetWorld();
		ConstructRegistry* reg  = world->GetConstructRegistry();
		ReplicationSystem* repl = world->GetReplicationSystem();

		const int64_t prefabIDRaw = GetCharacterPrefab(soul);
		const uint16_t typeHash   = ReflectionRegistry::ConstructTypeHashFromName("PlayerConstruct");

		// Stagger spawn positions so players don't overlap.
		const SimFloat spawnX = SimFloat((SpawnCounter % 2 == 0) ? -2.0f : 2.0f);
		++SpawnCounter;

		// Use PreInit callable so spawn position is set BEFORE Initialize() seeds JoltCharacter.
		auto* player = reg->Create<PlayerConstruct>(world, [&](PlayerConstruct* p)
		{
			p->SetOwnerSoul(&soul);
			p->SpawnPosX = spawnX;
			p->SpawnPosY = SimFloat(5.0f);
			p->SpawnPosZ = SimFloat(0.0f);
		});

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
			const uint32_t spawnFrame = world->GetLogicThread()
											? world->GetLogicThread()->GetLastCompletedFrame()
											: 0;
			ref = reg->AllocateNetRef(player, soul.GetOwnerID(), manifest, typeHash, prefabIDRaw, spawnFrame);
		}

		soul.ClaimBody(ref);
		LOG_INFO_F("[ArenaMode] Spawned body for ownerID=%u at (%.1f,5,0) (networked=%s)",
				   soul.GetOwnerID(), spawnX.ToFloat(), repl ? "yes" : "no");
	}
};

