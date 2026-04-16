#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "FieldProxy.h"

// Validates FieldProxy<float, Wide> — the unconditional AVX2 8-wide store path.
// Used by engine sweep passes that update all 8 entities in a cache lane.
// Guarded by __AVX2__ — skipped on builds without AVX2 support.
TEST(FieldProxy_WideStore)
{
	(void)Engine;

#ifndef __AVX2__
	SKIP_TEST("AVX2 not available in this build (ENABLE_AVX2=OFF)");
#else
	alignas(32) float    data[16] = {};
	alignas(32) int32_t flags[16] = {};

	// --- Wide store: all 8 lanes written unconditionally ---
	{
		FieldProxy<float, FieldWidth::Wide> proxy;
		// startCount=8 → mask has all 8 lanes active (though Wide ignores the mask on store)
		proxy.Bind(data, flags, 0, 8);

		proxy = 7.0f; // broadcasts scalar to all 8 lanes

		for (int i = 0; i < 8; ++i)
			ASSERT_EQ(data[i], 7.0f);
	}

	// --- Wide store: partial entity count in mask doesn't restrict Wide (unconditional) ---
	// Wide stores all 8 lanes even if count < 8. WideMask is the correct tier for partial.
	{
		alignas(32) float partial[8] = {0, 0, 0, 0, 0, 0, 0, 0};
		FieldProxy<float, FieldWidth::Wide> proxy;
		proxy.Bind(partial, flags, 0, 5); // only 5 "active" entities

		proxy = 3.0f;

		// Wide writes ALL 8 — this is intentional (WideMask handles masking)
		for (int i = 0; i < 8; ++i)
			ASSERT_EQ(partial[i], 3.0f);
	}

	// --- Wide +=, with dirty bits set on all 8 lanes ---
	{
		for (int i = 0; i < 8; ++i) data[i] = static_cast<float>(i);
		for (int i = 0; i < 8; ++i) flags[i] = 0;

		FieldProxy<float, FieldWidth::Wide> proxy;
		proxy.Bind(data, flags, 0, 8);
		proxy += 10.0f;

		for (int i = 0; i < 8; ++i)
			ASSERT_EQ(data[i], static_cast<float>(i) + 10.0f);

		constexpr int32_t DirtyBit = static_cast<int32_t>(1u << 30);
		for (int i = 0; i < 8; ++i)
			ASSERT((flags[i] & DirtyBit) != 0);
	}
#endif
}
