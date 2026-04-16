#include "TestFramework.h"
#include "TrinyxEngine.h"
#include <atomic>

// Validates that multiple sequential Engine.Spawn() calls all execute exactly once.
// Catches lambda payload corruption or double-execution bugs in the job queue.
RUNTIME_TEST(Jobs_MultiDispatch)
{
	static std::atomic<int> total{0};
	total.store(0, std::memory_order_relaxed);

	constexpr int Rounds = 5;
	for (int i = 0; i < Rounds; ++i)
	{
		Engine.Spawn([](uint32_t)
		{
			total.fetch_add(1, std::memory_order_relaxed);
		});
	}

	ASSERT_EQ(total.load(std::memory_order_acquire), Rounds);
}
