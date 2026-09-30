#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "FieldProxy.h"

// Validates FieldProxy<float, WideMask> — masked store at tail of a chunk.
// Used when fewer than kSIMDWide32Lanes entity slots are active in a lane.
// The mask is computed from startCount in Bind(); only lanes [0, startCount)
// are written. Lanes [startCount, kSIMDWide32Lanes) must remain unchanged.
// Guarded by TNX_HAS_AVX2 — skipped on builds without AVX2 support.
TEST(FieldProxy_WideMaskStore)
{
	(void)Engine;

#ifndef TNX_HAS_AVX2
	SKIP_TEST("AVX2 not available in this build (ENABLE_AVX2=OFF)");
#else
	alignas(FIELD_ARRAY_ALIGNMENT) SimFloat data[kSIMDWide32Lanes] = {};
	alignas(FIELD_ARRAY_ALIGNMENT) int32_t flags[kSIMDWide32Lanes] = {};

	// --- 5-active-lane masked store: lanes 0-4 written, rest untouched ---
	{
		FieldProxy<SimFloat, FieldWidth::WideMask> proxy;
		proxy.Bind(data, flags, 0, 5);

		proxy = SimFloat(9.9f);

		for (int i = 0; i < 5; ++i)
			ASSERT_EQ(data[i], SimFloat(9.9f));

		for (int i = 5; i < kSIMDWide32Lanes; ++i)
			ASSERT_EQ(data[i], SimFloat(0.0f)); // untouched — key invariant
	}

	// --- Full-lane masked store behaves like Wide ---
	{
		alignas(FIELD_ARRAY_ALIGNMENT) SimFloat full[kSIMDWide32Lanes] = {};
		for (int i = 0; i < kSIMDWide32Lanes; ++i)
			full[i] = static_cast<SimFloat>(i + 1);
		FieldProxy<SimFloat, FieldWidth::WideMask> proxy;
		proxy.Bind(full, flags, 0, kSIMDWide32Lanes);

		proxy = SimFloat(0.0f);

		for (int i = 0; i < kSIMDWide32Lanes; ++i)
			ASSERT_EQ(full[i], SimFloat(0.0f));
	}

	// --- 1-active-lane: only the first element written ---
	{
		alignas(FIELD_ARRAY_ALIGNMENT) SimFloat single[kSIMDWide32Lanes] = {};
		for (int i = 0; i < kSIMDWide32Lanes; ++i)
			single[i] = static_cast<SimFloat>(i);
		FieldProxy<SimFloat, FieldWidth::WideMask> proxy;
		proxy.Bind(single, flags, 0, 1);

		proxy = SimFloat(42.0f);

		ASSERT_EQ(single[0], SimFloat(42.0f));
		for (int i = 1; i < kSIMDWide32Lanes; ++i)
			ASSERT_EQ(single[i], static_cast<SimFloat>(i)); // unchanged
	}

	// --- Masked += ---
	{
		alignas(FIELD_ARRAY_ALIGNMENT) SimFloat addData[kSIMDWide32Lanes] = {};
		for (int i = 0; i < kSIMDWide32Lanes; ++i)
			addData[i] = SimFloat(10.0f);
		FieldProxy<SimFloat, FieldWidth::WideMask> proxy;
		proxy.Bind(addData, flags, 0, 3); // 3 active lanes

		proxy += SimFloat(5.0f);

		for (int i = 0; i < 3; ++i)
			ASSERT_EQ(addData[i], SimFloat(15.0f));
		for (int i = 3; i < kSIMDWide32Lanes; ++i)
			ASSERT_EQ(addData[i], SimFloat(10.0f)); // untouched
	}
#endif
}
