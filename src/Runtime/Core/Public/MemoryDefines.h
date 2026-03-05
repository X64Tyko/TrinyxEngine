#pragma once
#include <cstddef>
#include <cstdint>

// Memory alignment configuration for SIMD field arrays
//
// 32-byte alignment:
//   - Minimum for AVX2 SIMD operations
//   - ~25% of loads may cross cache line boundaries (~0.02-0.18ms penalty at 100k-1M entities)
//   - Lower memory overhead (~15 bytes avg padding per field array)
//
// 64-byte alignment:
//   - Cache line aligned, zero cache line splits
//   - Maximum performance (zero split penalty)
//   - Higher memory overhead (~31 bytes avg padding per field array)
//
// Configure via CMake: -DTNX_ALIGN_64=ON/OFF

#ifdef TNX_ALIGN_64
constexpr size_t FIELD_ARRAY_ALIGNMENT = 64;
#else
constexpr size_t FIELD_ARRAY_ALIGNMENT = 32;
#endif

// Maximum number of temporal/volatile components per slab (one per CacheTier).
// Determines:
//   - Number of available component-presence bits in TemporalFlags (bits 0..MAX_CACHE_COMPONENTS-1)
//   - Number of memory slots in each partition zone
//   - Capped by int32 Flags layout: 32 bits - 1 (Active) - 1 (Dirty) - 1 (reserved) = 29
// Will eventually be configurable via CMake (-DTNX_MAX_CACHE_COMPONENTS=N).
// Increase requires recompile; no runtime toggle.
constexpr uint8_t MAX_CACHE_COMPONENTS = 29;