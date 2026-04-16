#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "FieldProxy.h"

// Validates FieldProxy<float, Scalar> — the single-entity scalar update path.
// This is the most commonly exercised path (Construct-side entity writes).
// Tests: store via operator=, load via Value(), dirty bit marking, Advance() cursor.
TEST(FieldProxy_ScalarStoreLoad)
{
	(void)Engine;

	// Raw backing arrays — simulate what BuildFieldArrayTable provides
	alignas(32) float data[16]    = {};
	alignas(32) int32_t flags[16] = {};

	// --- Basic store and load ---
	{
		FieldProxy<float, FieldWidth::Scalar> proxy;
		proxy.Bind(data, flags, 0);

		proxy = 3.14f;
		ASSERT_EQ(proxy.Value(), 3.14f);
		ASSERT_EQ(data[0], 3.14f);
	}

	// --- Dirty bit set on store ---
	{
		FieldProxy<float, FieldWidth::Scalar> proxy;
		proxy.Bind(data, flags, 0);
		flags[0] = 0; // clear first

		proxy = 1.0f;

		constexpr int32_t DirtyBit        = static_cast<int32_t>(1u << 30);
		constexpr int32_t DirtiedFrameBit = static_cast<int32_t>(1u << 29);
		ASSERT((flags[0] & DirtyBit) != 0);
		ASSERT((flags[0] & DirtiedFrameBit) != 0);
	}

	// --- Advance cursor ---
	{
		data[0] = 10.0f;
		data[1] = 20.0f;
		data[2] = 30.0f;

		FieldProxy<float, FieldWidth::Scalar> proxy;
		proxy.Bind(data, flags, 0);
		ASSERT_EQ(proxy.Value(), 10.0f);

		proxy.Advance(1);
		ASSERT_EQ(proxy.Value(), 20.0f);

		proxy.Advance(1);
		ASSERT_EQ(proxy.Value(), 30.0f);
	}

	// --- Compound assignment operators ---
	{
		FieldProxy<float, FieldWidth::Scalar> proxy;
		proxy.Bind(data, flags, 0);
		data[0] = 10.0f;

		proxy += 5.0f;
		ASSERT_EQ(proxy.Value(), 15.0f);

		proxy -= 3.0f;
		ASSERT_EQ(proxy.Value(), 12.0f);

		proxy *= 2.0f;
		ASSERT_EQ(proxy.Value(), 24.0f);

		proxy /= 4.0f;
		ASSERT_EQ(proxy.Value(), 6.0f);
	}

	// --- Comparison operators (no dirty marking) ---
	{
		FieldProxy<float, FieldWidth::Scalar> proxy;
		proxy.Bind(data, flags, 0);
		data[0]  = 5.0f;
		flags[0] = 0;

		// Comparisons should not mark dirty
		[[maybe_unused]] auto gt = proxy > 4.0f;
		[[maybe_unused]] auto lt = proxy < 6.0f;
		[[maybe_unused]] auto eq = proxy == 5.0f;
		ASSERT_EQ(flags[0], 0); // no dirty bits set by comparisons
	}
}
