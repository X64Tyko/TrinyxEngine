#include "TestFramework.h"
#include "FlatMap.h"

// Validates that FlatMap maintains sorted key order after insertions in random order.
// The archetype lookup depends on sorted order for binary-search find().
TEST(FlatMap_OrderPreserved)
{
	FlatMap<int, int> Map;

	Map.insert_or_assign(50, 5);
	Map.insert_or_assign(10, 1);
	Map.insert_or_assign(30, 3);
	Map.insert_or_assign(20, 2);
	Map.insert_or_assign(40, 4);

	// Iterate and verify ascending key order
	int prev = -1;
	for (auto it = Map.begin(); it != Map.end(); ++it)
	{
		ASSERT(it->first > prev);
		prev = it->first;
	}

	ASSERT_EQ(Map.size(), 5u);
}
