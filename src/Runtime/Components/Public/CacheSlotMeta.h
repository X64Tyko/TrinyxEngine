#pragma once
#include "TemporalFlagBits.h"
#include "ComponentView.h"
#include "SchemaReflector.h"

/// @brief SoA component for per-entity status flags. Slot 0 of the temporal slab.
///
/// Wraps a single int32_t field and exposes typed bitwise operators over
/// @ref TemporalFlagBits. The GPU predicate pass reads @c Active (bit 31) to
/// drive the scatter compaction. @c Dirty (bit 30) accumulates until render clears.
/// @c DirtiedFrame (bit 29) is set per-frame and cleared at frame start.
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct CacheSlotMeta : ComponentView<CacheSlotMeta, WIDTH>
{
	TNX_TEMPORAL_FIELDS(CacheSlotMeta, Logic, Flags)

	IntProxy<WIDTH> Flags;

	static uint8_t GetTemporalIndex() { return 0; }

	FORCE_INLINE CacheSlotMeta& operator&=(TemporalFlagBits flag)
	{
		if constexpr (WIDTH == FieldWidth::Scalar)
			Flags = Flags.Value() & static_cast<int32_t>(flag);
		else
			Flags = _mm256_and_si256(Flags, _mm256_set1_epi32(static_cast<int32_t>(flag)));

		return *this;
	}

	FORCE_INLINE CacheSlotMeta& operator|=(TemporalFlagBits flag)
	{
		if constexpr (WIDTH == FieldWidth::Scalar)
			Flags = Flags.Value() | static_cast<int32_t>(flag);
		else
			Flags = _mm256_or_si256(Flags, _mm256_set1_epi32(static_cast<int32_t>(flag)));

		return *this;
	}
};

TNX_REGISTER_COMPONENT(CacheSlotMeta)
