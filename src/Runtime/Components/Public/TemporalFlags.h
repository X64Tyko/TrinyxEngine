#pragma once
#include "ComponentView.h"
#include "SchemaReflector.h"

enum class TemporalFlagBits : int32_t
{
    Active = 1 << 31
};

// EntityFlags Component - Flags for entity behavior
// Aligned to 32 bytes for GPU upload
template<FieldWidth WIDTH = FieldWidth::Scalar>
struct TemporalFlags : ComponentView<TemporalFlags, WIDTH>
{
    STRIGID_TEMPORAL_FIELDS(TemporalFlags, Flags)
    
    IntProxy<WIDTH> Flags;
    
    /*
    __forceinline TemporalFlags& operator&=(TemporalFlagBits flag)
    {
        if constexpr (WIDTH == FieldWidth::Scalar)
            Flags &= static_cast<int32_t>(flag);
        
        return *this;
    }
    */
};

STRIGID_REGISTER_COMPONENT(TemporalFlags)