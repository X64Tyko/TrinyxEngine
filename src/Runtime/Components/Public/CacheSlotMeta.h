#pragma once
#include "ComponentView.h"
#include "SchemaReflector.h"

enum class TemporalFlagBits : int32_t
{
	Active = static_cast<int32_t>(1u << 31), ///< Entity is alive — GPU predicate reads this
	Dirty  = static_cast<int32_t>(1u << 30), ///< Entity data changed this tick — triggers GPU upload
	// Bits 29..0 available for game-layer flags (physics flags, visibility, team bits, etc.)
};

// EntityFlags Component - Flags for entity behavior
// Aligned to 32 bytes for GPU upload
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