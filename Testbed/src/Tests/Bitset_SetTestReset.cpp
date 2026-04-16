#include "TestFramework.h"
#include "FixedBitset.h"

// Validates FixedBitset<256> core operations: set, test, reset, count.
// FixedBitset is the backing type for ComponentSignature — correctness here
// is foundational to the entire component/archetype system.
TEST(Bitset_SetTestReset)
{
	FixedBitset<256> bs;

	// Initially all clear
	ASSERT(!bs.test(0));
	ASSERT(!bs.test(63));
	ASSERT(!bs.test(64));
	ASSERT(!bs.test(255));
	ASSERT_EQ(static_cast<int>(bs.count()), 0);

	// Set specific bits
	bs.set(0);
	bs.set(63);
	bs.set(64);
	bs.set(255);
	ASSERT(bs.test(0));
	ASSERT(bs.test(63));
	ASSERT(bs.test(64));
	ASSERT(bs.test(255));
	ASSERT_EQ(static_cast<int>(bs.count()), 4);

	// Reset one bit
	bs.reset(63);
	ASSERT(!bs.test(63));
	ASSERT_EQ(static_cast<int>(bs.count()), 3);

	// Other bits unaffected
	ASSERT(bs.test(0));
	ASSERT(bs.test(64));
	ASSERT(bs.test(255));
}
