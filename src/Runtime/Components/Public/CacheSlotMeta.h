#pragma once
#include "ComponentView.h"
#include "SchemaReflector.h"

enum class TemporalFlagBits : int32_t
{
	Active       = static_cast<int32_t>(1u << 31), ///< Entity ticks and renders — GPU predicate reads this
	Dirty        = static_cast<int32_t>(1u << 30), ///< Entity data changed — accumulates until render clears
	DirtiedFrame = static_cast<int32_t>(1u << 29), ///< Entity dirtied THIS frame — cleared at frame start, used for per-frame logic reset
	Replicated   = static_cast<int32_t>(1u << 28), ///< This entity replicates
	Alive        = static_cast<int32_t>(1u << 27), ///< Entity exists in the world — data is valid, StateCorrections apply. Set on spawn, cleared on destroy. Active implies Alive.

	depthMask = 0xF << 24, ///< 4 bits for attachment depth

	// Used for ConstructView bound entities
	PrePhysSkip  = static_cast<int32_t>(1u << 23), ///< if 1 Disable PrePhysics sweep
	PostPhysSkip = static_cast<int32_t>(1u << 22), ///< if 1 Disable PostPhysics sweep
	ScalarSkip   = static_cast<int32_t>(1u << 21), ///< if 1 Disable ScalarUpdate sweep
	ASleep       = static_cast<int32_t>(1u << 20), ///< if 1 Disable in Jolt
	// Bits 19..0 available for game-layer flags
};

FORCE_INLINE TemporalFlagBits operator|(TemporalFlagBits lhs, TemporalFlagBits rhs)
{
	return static_cast<TemporalFlagBits>(static_cast<int32_t>(lhs) | static_cast<int32_t>(rhs));
}

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
		if constexpr (WIDTH == FieldWidth::Scalar) Flags = Flags.Value() & static_cast<int32_t>(flag);
		else Flags                                       = _mm256_and_si256(Flags, _mm256_set1_epi32(static_cast<int32_t>(flag)));

		return *this;
	}

	FORCE_INLINE CacheSlotMeta& operator|=(TemporalFlagBits flag)
	{
		if constexpr (WIDTH == FieldWidth::Scalar) Flags = Flags.Value() | static_cast<int32_t>(flag);
		else Flags                                       = _mm256_or_si256(Flags, _mm256_set1_epi32(static_cast<int32_t>(flag)));

		return *this;
	}
};

TNX_REGISTER_COMPONENT(CacheSlotMeta)
