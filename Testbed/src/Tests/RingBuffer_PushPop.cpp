#include "TestFramework.h"
#include "TrinyxMPMCRing.h"

// Validates TrinyxMPMCRing basic MPMC correctness:
// push 5 values, pop them back in FIFO order, buffer is empty after.
TEST(RingBuffer_PushPop)
{
	TrinyxMPMCRing<int> rb;
	ASSERT(rb.Initialize(8));

	for (int i = 0; i < 5; ++i)
		ASSERT(rb.TryPush(i));

	for (int i = 0; i < 5; ++i)
	{
		int val = -1;
		ASSERT(rb.TryPop(val));
		ASSERT_EQ(val, i);
	}

	// Buffer is now empty
	int dummy = -1;
	ASSERT(!rb.TryPop(dummy));
	ASSERT(rb.IsEmpty());
}
