#ifdef TNX_ENABLE_NETWORK

#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "World.h"
#include "ConstructRegistry.h"
#include "ReplicationSystem.h"
#include "ReflectionRegistry.h"
#include "Logger.h"
#include "PlayerConstruct.h"

// Spawns a PlayerConstruct via the replication path (as a server would for a client).
// Exercises: ConstructRegistry::Create with SetOwnerSoul, ReplicationSystem::RegisterConstruct,
//            ConstructRef round-trip, authority model.
//
// Requires: TNX_ENABLE_NETWORK=ON
// Compare with: Spawn_PlayerConstruct (standalone, no replication)
RUNTIME_TEST(Net_PlayerConstruct)
{
	WorldBase* world = Engine.GetDefaultWorld();
	ASSERT(world != nullptr);

	ReplicationSystem* repl     = world->GetReplicationSystem();
	ConstructRegistry* reg      = world->GetConstructRegistry();
	const uint16_t     typeHash = ReflectionRegistry::ConstructTypeHashFromName("PlayerConstruct");

	/*
	 * TODO: Net_PlayerConstruct — full loopback PIE test.
	 *
	 * Goal: prove that a server-side PlayerConstruct spawned via RegisterConstruct
	 * replicates to a client and produces a matching ConstructRef on both sides.
	 *
	 * Steps (once PIE loopback session management is stable):
	 *   1. Start a loopback PIE session (AuthorityNet + OwnerNet in-process).
	 *   2. Fire a PlayerBeginRequest from the client soul.
	 *   3. Server's TestNetGameMode::OnPlayerBeginRequest runs:
	 *      - Creates PlayerConstruct at (2, 5, 0)
	 *      - Calls repl->RegisterConstruct(reg, player, ownerID, typeHash, 0)
	 *      - Returns ConstructRef to client via Soul::ClaimBody
	 *   4. ASSERT client's soul reports a valid body ref.
	 *   5. ASSERT PlayerConstruct's transform is near (2, 5, 0) after one physics step.
	 *   6. Despawn and verify handle recycling.
	 *
	 * Blocked by: PIE session bootstrap API stabilization.
	 */

	(void)repl; (void)reg; (void)typeHash;
	LOG_ENG_ALWAYS("[Net_PlayerConstruct] Networked PlayerConstruct test — pending PIE session bootstrap");

	// Placeholder: at minimum assert world is ready for replication
	ASSERT(repl != nullptr);
}

#endif // TNX_ENABLE_NETWORK
