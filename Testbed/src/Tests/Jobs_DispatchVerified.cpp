#include "TestFramework.h"
#include "TrinyxEngine.h"
#include <atomic>

// Validates that Engine.Spawn() actually executes its lambda before returning.
// SpawnAndWait is synchronous — if the lambda hasn't run when Spawn() returns,
// the engine's spawn contract is broken and all spawning code is unreliable.
RUNTIME_TEST(Jobs_DispatchVerified)
{
	static std::atomic<int> counter{0};
	counter.store(0, std::memory_order_relaxed);

	Engine.Spawn([](uint32_t)
	{
		counter.fetch_add(1, std::memory_order_relaxed);
	});

	ASSERT_EQ(counter.load(std::memory_order_acquire), 1);
}
