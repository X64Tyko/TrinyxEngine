#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "FieldProxy.h"

// Validates FieldProxy<float, Wide> — the unconditional wide store path.
// Used by engine sweep passes that update all entities in a cache lane.
// Lane count is kSIMDWide32Lanes (8 on AVX2, 16 on AVX-512).
// Guarded by __AVX2__ — skipped on builds without AVX2 support.
TEST(FieldProxy_WideStore)
{
	(void)Engine;

#ifndef __AVX2__
	SKIP_TEST("AVX2 not available in this build (ENABLE_AVX2=OFF)");
#else
	alignas(FIELD_ARRAY_ALIGNMENT) SimFloat data[kSIMDWide32Lanes * 2] = {};
	alignas(FIELD_ARRAY_ALIGNMENT) int32_t flags[kSIMDWide32Lanes * 2] = {};

	// --- Wide store: all lanes written unconditionally ---
	{
		FieldProxy<SimFloat, FieldWidth::Wide> proxy;
		proxy.Bind(data, flags, 0, kSIMDWide32Lanes);

		proxy = SimFloat(7.0f);

		for (int i = 0; i < kSIMDWide32Lanes; ++i)
			ASSERT_EQ(data[i], SimFloat(7.0f));
	}

	// --- Wide store: partial entity count in mask doesn't restrict Wide (unconditional) ---
	// Wide stores all lanes even if count < kSIMDWide32Lanes. WideMask is the correct tier for partial.
	{
		alignas(FIELD_ARRAY_ALIGNMENT) SimFloat partial[kSIMDWide32Lanes] = {};
		FieldProxy<SimFloat, FieldWidth::Wide> proxy;
		proxy.Bind(partial, flags, 0, 5); // only 5 "active" entities

		proxy = SimFloat(3.0f);

		// Wide writes ALL lanes — this is intentional (WideMask handles masking)
		for (int i = 0; i < kSIMDWide32Lanes; ++i)
			ASSERT_EQ(partial[i], SimFloat(3.0f));
	}

	// --- Wide +=, with dirty bits set on all lanes ---
	{
		for (int i = 0; i < kSIMDWide32Lanes; ++i)
			data[i] = static_cast<SimFloat>(i);
		for (int i = 0; i < kSIMDWide32Lanes; ++i)
			flags[i] = 0;

		FieldProxy<SimFloat, FieldWidth::Wide> proxy;
		proxy.Bind(data, flags, 0, kSIMDWide32Lanes);
		proxy += SimFloat(10.0f);

		for (int i = 0; i < kSIMDWide32Lanes; ++i)
			ASSERT_EQ(data[i], static_cast<SimFloat>(i) + SimFloat(10.0f));

		constexpr int32_t DirtyBit = static_cast<int32_t>(1u << 30);
		for (int i = 0; i < kSIMDWide32Lanes; ++i)
			ASSERT((flags[i] & DirtyBit) != 0);
	}
#endif
}
