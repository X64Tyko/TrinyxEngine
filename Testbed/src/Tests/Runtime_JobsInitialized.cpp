#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Logger.h"

// Validates that the job system is fully initialized by the time runtime tests run.
// This is the baseline runtime test — if this fails, all other runtime tests are suspect.
RUNTIME_TEST(Runtime_JobsInitialized)
{
	ASSERT(Engine.GetJobsInitialized());
}
