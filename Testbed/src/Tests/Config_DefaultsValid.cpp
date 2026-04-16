#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "EngineConfig.h"

// Validates that after engine initialization all critical config fields have been
// resolved from Unset to real values, and that helper methods return safe values.
TEST(Config_DefaultsValid)
{
	const EngineConfig* Cfg = Engine.GetConfig();
	ASSERT(Cfg != nullptr);

	// FixedUpdateHz must be resolved — Unset (-1) would produce a negative step time
	ASSERT(Cfg->FixedUpdateHz != EngineConfig::Unset);
	ASSERT(Cfg->GetFixedStepTime() > 0.0);

	// GetNetworkStepTime() must not divide by zero
	ASSERT(Cfg->GetNetworkStepTime() >= 0.0);

	// Entity capacities must be resolved
	ASSERT(Cfg->MAX_CACHED_ENTITIES != EngineConfig::Unset);
	ASSERT(Cfg->MAX_RENDERABLE_ENTITIES != EngineConfig::Unset);
	ASSERT(Cfg->MAX_JOLT_BODIES != EngineConfig::Unset);
}
