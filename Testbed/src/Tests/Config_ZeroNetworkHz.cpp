#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "EngineConfig.h"

// Validates that GetNetworkStepTime() does NOT divide by zero when NetworkUpdateHz is 0.
// API hole note: If a caller sets NetworkUpdateHz=0 explicitly, GetNetworkStepTime
// must return 0 (disabled), not infinity or NaN. The helper correctly guards this,
// but this test ensures the guard stays in place through refactors.
TEST(Config_ZeroNetworkHz)
{
	EngineConfig Cfg;
	Cfg.NetworkUpdateHz = 0;

	double stepTime = Cfg.GetNetworkStepTime();
	ASSERT(stepTime == 0.0); // 0 = networking disabled, not a crash
}
