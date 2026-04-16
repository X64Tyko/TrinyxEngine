#include "TestFramework.h"
#include "TrinyxRingBuffer.h"

// Validates strict FIFO ordering on TrinyxRingBuffer.
// Interleaves pushes and pops to prove the MPMC sequence counter stays coherent
// across partial drain/refill cycles.
TEST(RingBuffer_FifoOrder)
{
	TrinyxRingBuffer<int> rb;
	ASSERT(rb.Initialize(8));

	// Push 1-4, pop 1-2, push 5-6, pop 3-6
	for (int i = 1; i <= 4; ++i) ASSERT(rb.TryPush(i));

	int v = -1;
	ASSERT(rb.TryPop(v)); ASSERT_EQ(v, 1);
	ASSERT(rb.TryPop(v)); ASSERT_EQ(v, 2);

	ASSERT(rb.TryPush(5));
	ASSERT(rb.TryPush(6));

	ASSERT(rb.TryPop(v)); ASSERT_EQ(v, 3);
	ASSERT(rb.TryPop(v)); ASSERT_EQ(v, 4);
	ASSERT(rb.TryPop(v)); ASSERT_EQ(v, 5);
	ASSERT(rb.TryPop(v)); ASSERT_EQ(v, 6);

	ASSERT(rb.IsEmpty());
}
