#pragma once

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
// Configure via CMake: -DSTRIGID_ALIGN_64=ON/OFF

#ifdef STRIGID_ALIGN_64
    constexpr size_t FIELD_ARRAY_ALIGNMENT = 64;
#else
    constexpr size_t FIELD_ARRAY_ALIGNMENT = 32;
#endif
