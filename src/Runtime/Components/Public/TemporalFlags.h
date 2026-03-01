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
struct TemporalFlags : ComponentView<TemporalFlags, WIDTH>
{
	STRIGID_TEMPORAL_FIELDS(TemporalFlags, Flags)

	IntProxy<WIDTH> Flags;

	/*
    FORCE_INLINE TemporalFlags& operator&=(TemporalFlagBits flag)
    {
        if constexpr (WIDTH == FieldWidth::Scalar)
            Flags &= static_cast<int32_t>(flag);

        return *this;
    }
    */
};

STRIGID_REGISTER_COMPONENT(TemporalFlags)