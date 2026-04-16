#include "TestFramework.h"
#include "FlatMap.h"

// Validates FlatMap<int, int> core operations:
// insert_or_assign, find, operator[], update-in-place, erase, contains.
// FlatMap is the ordered sorted container backing Registry's archetype lookup.
TEST(FlatMap_BasicOps)
{
	FlatMap<int, int> Map;

	// Insert new entries
	Map.insert_or_assign(3, 30);
	Map.insert_or_assign(1, 10);
	Map.insert_or_assign(2, 20);

	ASSERT_EQ(Map.size(), 3u);

	// Find existing
	int* v = Map.find(1);
	ASSERT(v != nullptr);
	ASSERT_EQ(*v, 10);

	// Find missing
	ASSERT(Map.find(99) == nullptr);

	// contains
	ASSERT(Map.contains(2));
	ASSERT(!Map.contains(42));

	// Operator[] read
	ASSERT_EQ(Map[3], 30);

	// Update existing via insert_or_assign
	Map.insert_or_assign(1, 100);
	ASSERT_EQ(*Map.find(1), 100);
	ASSERT_EQ(Map.size(), 3u); // count unchanged

	// Erase
	ASSERT(Map.erase(2));
	ASSERT_EQ(Map.size(), 2u);
	ASSERT(!Map.contains(2));

	// Erase missing returns false
	ASSERT(!Map.erase(2));
}
