#include "TestFramework.h"
#include "TrinyxRingBuffer.h"

// Validates that TryPush returns false (not UB or block) when the ring buffer is full.
// Capacity 4 means exactly 4 items can be in-flight before the buffer saturates.
TEST(RingBuffer_Full)
{
	TrinyxRingBuffer<int> rb;
	ASSERT(rb.Initialize(4));

	// Fill to capacity
	ASSERT(rb.TryPush(10));
	ASSERT(rb.TryPush(20));
	ASSERT(rb.TryPush(30));
	ASSERT(rb.TryPush(40));

	// One more must fail
	ASSERT(!rb.TryPush(99));

	// Pop one, then a new push must succeed
	int val = -1;
	ASSERT(rb.TryPop(val));
	ASSERT_EQ(val, 10);
	ASSERT(rb.TryPush(50));
}
