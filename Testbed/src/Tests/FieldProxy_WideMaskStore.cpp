#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "FieldProxy.h"

// Validates FieldProxy<float, WideMask> — AVX2 masked store.
// Used at the tail of a chunk when fewer than 8 entity slots are active.
// The mask is computed from startCount in Bind(); only lanes [0, startCount)
// are written. Lanes [startCount, 8) must remain unchanged.
// Guarded by __AVX2__ — skipped on builds without AVX2 support.
TEST(FieldProxy_WideMaskStore)
{
	(void)Engine;

#ifndef TNX_HAS_AVX2
	SKIP_TEST("AVX2 not available in this build (ENABLE_AVX2=OFF)");
#else
	alignas(32) SimFloat data[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	alignas(32) int32_t flags[8]  = {};

	// --- 5-active-lane masked store: lanes 0-4 written, 5-7 untouched ---
	{
		FieldProxy<SimFloat, FieldWidth::WideMask> proxy;
		proxy.Bind(data, flags, 0, 5); // 5 active lanes

		proxy = SimFloat(9.9f);

		for (int i = 0; i < 5; ++i) ASSERT_EQ(data[i], SimFloat(9.9f));

		for (int i = 5; i < 8; ++i)
			ASSERT_EQ(data[i], 0.0f); // untouched — key invariant
	}

	// --- Full 8-lane masked store behaves like Wide ---
	{
		alignas(32) SimFloat full[8] = {1, 2, 3, 4, 5, 6, 7, 8};
		FieldProxy<SimFloat, FieldWidth::WideMask> proxy;
		proxy.Bind(full, flags, 0, 8);

		proxy = SimFloat(0.0f);

		for (int i = 0; i < 8; ++i)
			ASSERT_EQ(full[i], 0.0f);
	}

	// --- 1-active-lane: only the first element written ---
	{
		alignas(32) SimFloat single[8] = {0, 1, 2, 3, 4, 5, 6, 7};
		FieldProxy<SimFloat, FieldWidth::WideMask> proxy;
		proxy.Bind(single, flags, 0, 1);

		proxy = SimFloat(42.0f);

		ASSERT_EQ(single[0], 42.0f);
		for (int i = 1; i < 8; ++i) ASSERT_EQ(single[i], static_cast<SimFloat>(i)); // unchanged
	}

	// --- Masked += ---
	{
		alignas(32) SimFloat addData[8] = {10, 10, 10, 10, 10, 10, 10, 10};
		FieldProxy<SimFloat, FieldWidth::WideMask> proxy;
		proxy.Bind(addData, flags, 0, 3); // 3 active lanes

		proxy += SimFloat(5.0f);

		for (int i = 0; i < 3; ++i)
			ASSERT_EQ(addData[i], 15.0f);
		for (int i = 3; i < 8; ++i)
			ASSERT_EQ(addData[i], 10.0f); // untouched
	}
#endif
}
