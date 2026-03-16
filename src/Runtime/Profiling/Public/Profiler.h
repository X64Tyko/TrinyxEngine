#pragma once

// Tracy Profiler Integration
// Compile with TRACY_ENABLE to activate profiling
// Run the Tracy profiler GUI to capture and visualize traces
//
// Profiling Levels:
// 0 - Disabled (no profiling)
// 1 - Coarse (frame/system level only - low overhead, ~1-2% impact)
// 2 - Medium (includes subsystem zones - moderate overhead, ~5-10% impact)
// 3 - Fine (includes hot loop zones - high overhead, ~50%+ impact)
//
// Set TRACY_PROFILE_LEVEL in CMakeLists.txt or compiler flags
// Default is level 1 (coarse) when TRACY_ENABLE is defined

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>

// Determine profiling level (default to 1 if not specified)
#ifndef TRACY_PROFILE_LEVEL
#define TRACY_PROFILE_LEVEL 1
#endif

// Frame marker - always enabled when Tracy is on
#define TNX_FRAME_MARK() FrameMark

// Level 1: Coarse profiling (frame/system boundaries)
#if TRACY_PROFILE_LEVEL >= 1
#define TNX_ZONE_COARSE()                ZoneScoped
#define TNX_ZONE_COARSE_N(name)          ZoneScopedN(name)
#define TNX_ZONE_COARSE_C(color)         ZoneScopedC(color)
#define TNX_ZONE_COARSE_NC(name, color)  ZoneScopedNC(name, color)
#else
#define TNX_ZONE_COARSE()
#define TNX_ZONE_COARSE_N(name)
#define TNX_ZONE_COARSE_C(color)
#define TNX_ZONE_COARSE_NC(name, color)
#endif

// Level 2: Medium profiling (subsystems, larger functions)
#if TRACY_PROFILE_LEVEL >= 2
#define TNX_ZONE_MEDIUM()                ZoneScoped
#define TNX_ZONE_MEDIUM_N(name)          ZoneScopedN(name)
#define TNX_ZONE_MEDIUM_C(color)         ZoneScopedC(color)
#define TNX_ZONE_MEDIUM_NC(name, color)  ZoneScopedNC(name, color)
#else
#define TNX_ZONE_MEDIUM()
#define TNX_ZONE_MEDIUM_N(name)
#define TNX_ZONE_MEDIUM_C(color)
#define TNX_ZONE_MEDIUM_NC(name, color)
#endif

// Level 3: Fine profiling (hot loops, per-entity operations)
#if TRACY_PROFILE_LEVEL >= 3
#define TNX_ZONE_FINE()                  ZoneScoped
#define TNX_ZONE_FINE_N(name)            ZoneScopedN(name)
#define TNX_ZONE_FINE_C(color)           ZoneScopedC(color)
#define TNX_ZONE_FINE_NC(name, color)    ZoneScopedNC(name, color)
#else
#define TNX_ZONE_FINE()
#define TNX_ZONE_FINE_N(name)
#define TNX_ZONE_FINE_C(color)
#define TNX_ZONE_FINE_NC(name, color)
#endif

// Legacy macros (map to COARSE for compatibility)
#define TNX_ZONE()                       TNX_ZONE_COARSE()
#define TNX_ZONE_N(name)                 TNX_ZONE_COARSE_N(name)
#define TNX_ZONE_C(color)                TNX_ZONE_COARSE_C(color)
#define TNX_ZONE_NC(name, color)         TNX_ZONE_COARSE_NC(name, color)

// Zone with dynamic text (e.g., "Processing Entity 42")
#define TNX_ZONE_TEXT(text, size) ZoneText(text, size)

// Memory profiling
#define TNX_ALLOC(ptr, size) TracyAlloc(ptr, size)
#define TNX_FREE(ptr) TracyFree(ptr)
#define TNX_ALLOC_N(ptr, size, name) TracyAllocN(ptr, size, name)
#define TNX_FREE_N(ptr, name) TracyFreeN(ptr, name)

// Plots (for custom metrics like FPS, entity count, etc.)
#define TNX_PLOT(name, value) TracyPlot(name, value)

#else
// No-op macros when Tracy is disabled
#define TNX_FRAME_MARK()
#define TNX_ZONE()
#define TNX_ZONE_N(name)
#define TNX_ZONE_C(color)
#define TNX_ZONE_NC(name, color)
#define TNX_ZONE_COARSE()
#define TNX_ZONE_COARSE_N(name)
#define TNX_ZONE_COARSE_C(color)
#define TNX_ZONE_COARSE_NC(name, color)
#define TNX_ZONE_MEDIUM()
#define TNX_ZONE_MEDIUM_N(name)
#define TNX_ZONE_MEDIUM_C(color)
#define TNX_ZONE_MEDIUM_NC(name, color)
#define TNX_ZONE_FINE()
#define TNX_ZONE_FINE_N(name)
#define TNX_ZONE_FINE_C(color)
#define TNX_ZONE_FINE_NC(name, color)
#define TNX_ZONE_TEXT(text, size)
#define TNX_ALLOC(ptr, size)
#define TNX_FREE(ptr)
#define TNX_ALLOC_N(ptr, size, name)
#define TNX_FREE_N(ptr, name)
#define TNX_PLOT(name, value)
#endif

// Tracy color definitions (24-bit RGB)
#define TNX_COLOR_MEMORY    0xFF6B6B  // Red
#define TNX_COLOR_RENDERING 0x4ECDC4  // Cyan
#define TNX_COLOR_PHYSICS   0xFFC933  // Yellow
#define TNX_COLOR_LOGIC     0x9527F5  // Purple
#define TNX_COLOR_NETWORK   0xF38181  // Pink
#define TNX_COLOR_AUDIO     0x6600FF  // Purple
#define TNX_COLOR_WORKER    0x45B7D1  // Steel Blue
#define TNX_COLOR_JOLT      0xFF8040  // Orange