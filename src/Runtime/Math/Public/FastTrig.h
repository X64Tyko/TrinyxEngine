#pragma once

// Cross-platform force inline macro — used by VecMath.h and QuatMath.h.
#ifndef FORCE_INLINE
#ifdef _MSC_VER
#define FORCE_INLINE __forceinline
#else
#define FORCE_INLINE inline __attribute__((always_inline))
#endif
#endif
