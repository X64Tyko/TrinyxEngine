#pragma once
#include "ComponentView.h"
#include "SchemaReflector.h"

enum class TemporalFlagBits : int32_t
{
	Active       = static_cast<int32_t>(1u << 31), ///< Entity is alive — GPU predicate reads this
	Dirty        = static_cast<int32_t>(1u << 30), ///< Entity data changed — accumulates until render clears
	DirtiedFrame = static_cast<int32_t>(1u << 29), ///< Entity dirtied THIS frame — cleared at frame start, used for per-frame logic reset
	// Bits 28..0 available for game-layer flags
};

// Per-entity flags in the SoA slab. GPU predicate reads Active (bit 31).
// Dirty (bit 30) accumulates until render clears. DirtiedFrame (bit 29) is per-frame.
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct CacheSlotMeta : ComponentView<CacheSlotMeta, WIDTH>
{
	TNX_TEMPORAL_FIELDS(CacheSlotMeta, Logic, Flags)

	IntProxy<WIDTH> Flags;

	static uint8_t GetTemporalIndex() { return 0; }

    FORCE_INLINE CacheSlotMeta& operator&=(TemporalFlagBits flag)
	{
		if constexpr (WIDTH == FieldWidth::Scalar) Flags &= static_cast<int32_t>(flag);
		else Flags                                       = _mm256_and_si256(Flags, _mm256_set1_epi32(static_cast<int32_t>(flag)));

		return *this;
	}
};

TNX_REGISTER_COMPONENT(CacheSlotMeta)