#include "TestFramework.h"
#include "Signature.h"

// Validates Signature::Contains() logic used by ComponentQuery archetype matching.
// A superset must contain its subset; an equal set contains itself;
// a disjoint set must not be contained.
TEST(Bitset_SignatureContains)
{
	Signature Full, Partial, Disjoint;

	// Full = bits 0, 1, 2
	Full.Set(0); Full.Set(1); Full.Set(2);

	// Partial = bits 0, 1 only
	Partial.Set(0); Partial.Set(1);

	// Disjoint = bit 5 only
	Disjoint.Set(5);

	// Superset contains subset
	ASSERT(Full.Contains(Partial));

	// Equal set contains itself
	ASSERT(Full.Contains(Full));
	ASSERT(Partial.Contains(Partial));

	// Subset does NOT contain superset
	ASSERT(!Partial.Contains(Full));

	// Disjoint containment fails both ways
	ASSERT(!Full.Contains(Disjoint));
	ASSERT(!Disjoint.Contains(Full));

	// Empty signature is contained by everything
	Signature Empty;
	ASSERT(Full.Contains(Empty));
	ASSERT(Partial.Contains(Empty));
}
