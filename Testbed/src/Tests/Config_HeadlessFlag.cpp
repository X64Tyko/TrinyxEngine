#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "EngineConfig.h"

// Validates that the compile-time TNX_HEADLESS flag implies the runtime config is headless.
// The reverse is NOT checked: running with --headless at runtime is valid even in a
// non-TNX_HEADLESS build (e.g. CI running headless on a build without the compile-time flag).
// What must always hold: a TNX_HEADLESS build can never enter graphics mode.
TEST(Config_HeadlessFlag)
{
	const EngineConfig* Cfg = Engine.GetConfig();
	ASSERT(Cfg != nullptr);

#ifdef TNX_HEADLESS
	// Headless build must always run headless — no graphics even if config omits the flag.
	ASSERT(Cfg->Headless == true);
#endif
	// Non-headless build: Cfg->Headless depends on the --headless runtime argument;
	// both values are valid. No assertion needed.
}
