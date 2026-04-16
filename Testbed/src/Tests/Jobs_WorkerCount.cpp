#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "TrinyxJobs.h"

// Validates that the job system reports at least 1 worker by the time runtime tests run.
// A worker count of 0 means all Dispatch calls would stall forever — catastrophic for
// any physics or logic workload.
//
// API note: GetWorkerCount() is a free function in TrinyxJobs namespace. There is
// currently no Engine-level accessor for this, so callers must include TrinyxJobs.h.
// Consider exposing Engine.GetWorkerCount() for convenience.
RUNTIME_TEST(Jobs_WorkerCount)
{
	ASSERT(Engine.GetJobsInitialized());
	uint32_t Workers = TrinyxJobs::GetWorkerCount();
	ASSERT(Workers >= 1);
}
