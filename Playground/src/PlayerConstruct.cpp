#include "PlayerConstruct.h"

#include "ReflectionRegistry.h"
#include "SchemaReflector.h"

// Register PlayerConstruct for networked Construct replication.
// This enables ReplicationSystem::HandleConstructSpawn to instantiate
// a client-side PlayerConstruct via InitializeForReplication.
TNX_REGISTER_CONSTRUCT(PlayerConstruct)
