#pragma once
#include <cmath>

// Cross-platform force inline macro
#ifdef _MSC_VER
#define FORCE_INLINE __forceinline
#else
#define FORCE_INLINE inline __attribute__((always_inline))
#endif

// Fast trig functions. By default these map to the standard library.
// A LUT-based implementation can be swapped in by defining TNX_USE_FAST_TRIG
// and providing the implementation.

#if 0
// Fast LUT-based trig – not yet implemented.
// When added, the tables must be original work (not derived from GPL sources).
#endif

// Fallback to standard library functions
FORCE_INLINE float FastSin(float x) { return std::sin(x); }
FORCE_INLINE double FastSin(double x) { return std::sin(x); }
FORCE_INLINE float FastCos(float x) { return std::cos(x); }
FORCE_INLINE double FastCos(double x) { return std::cos(x); }
FORCE_INLINE float FastTan(float x) { return std::tan(x); }
FORCE_INLINE double FastTan(double x) { return std::tan(x); }
