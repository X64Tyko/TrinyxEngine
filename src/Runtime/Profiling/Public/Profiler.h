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
#define STRIGID_FRAME_MARK() FrameMark

// Level 1: Coarse profiling (frame/system boundaries)
#if TRACY_PROFILE_LEVEL >= 1
#define STRIGID_ZONE_COARSE() ZoneScoped
#define STRIGID_ZONE_COARSE_N(name) ZoneScopedN(name)
#define STRIGID_ZONE_COARSE_C(color) ZoneScopedC(color)
#else
#define STRIGID_ZONE_COARSE()
#define STRIGID_ZONE_COARSE_N(name)
#define STRIGID_ZONE_COARSE_C(color)
#endif

// Level 2: Medium profiling (subsystems, larger functions)
#if TRACY_PROFILE_LEVEL >= 2
#define STRIGID_ZONE_MEDIUM() ZoneScoped
#define STRIGID_ZONE_MEDIUM_N(name) ZoneScopedN(name)
#define STRIGID_ZONE_MEDIUM_C(color) ZoneScopedC(color)
#else
#define STRIGID_ZONE_MEDIUM()
#define STRIGID_ZONE_MEDIUM_N(name)
#define STRIGID_ZONE_MEDIUM_C(color)
#endif

// Level 3: Fine profiling (hot loops, per-entity operations)
#if TRACY_PROFILE_LEVEL >= 3
#define STRIGID_ZONE_FINE() ZoneScoped
#define STRIGID_ZONE_FINE_N(name) ZoneScopedN(name)
#define STRIGID_ZONE_FINE_C(color) ZoneScopedC(color)
#else
#define STRIGID_ZONE_FINE()
#define STRIGID_ZONE_FINE_N(name)
#define STRIGID_ZONE_FINE_C(color)
#endif

// Legacy macros (map to COARSE for compatibility)
#define STRIGID_ZONE() STRIGID_ZONE_COARSE()
#define STRIGID_ZONE_N(name) STRIGID_ZONE_COARSE_N(name)
#define STRIGID_ZONE_C(color) STRIGID_ZONE_COARSE_C(color)

// Zone with dynamic text (e.g., "Processing Entity 42")
#define STRIGID_ZONE_TEXT(text, size) ZoneText(text, size)

// Memory profiling
#define STRIGID_ALLOC(ptr, size) TracyAlloc(ptr, size)
#define STRIGID_FREE(ptr) TracyFree(ptr)
#define STRIGID_ALLOC_N(ptr, size, name) TracyAllocN(ptr, size, name)
#define STRIGID_FREE_N(ptr, name) TracyFreeN(ptr, name)

// Plots (for custom metrics like FPS, entity count, etc.)
#define STRIGID_PLOT(name, value) TracyPlot(name, value)

#else
// No-op macros when Tracy is disabled
#define STRIGID_FRAME_MARK()
#define STRIGID_ZONE()
#define STRIGID_ZONE_N(name)
#define STRIGID_ZONE_C(color)
#define STRIGID_ZONE_COARSE()
#define STRIGID_ZONE_COARSE_N(name)
#define STRIGID_ZONE_COARSE_C(color)
#define STRIGID_ZONE_MEDIUM()
#define STRIGID_ZONE_MEDIUM_N(name)
#define STRIGID_ZONE_MEDIUM_C(color)
#define STRIGID_ZONE_FINE()
#define STRIGID_ZONE_FINE_N(name)
#define STRIGID_ZONE_FINE_C(color)
#define STRIGID_ZONE_TEXT(text, size)
#define STRIGID_ALLOC(ptr, size)
#define STRIGID_FREE(ptr)
#define STRIGID_ALLOC_N(ptr, size, name)
#define STRIGID_FREE_N(ptr, name)
#define STRIGID_PLOT(name, value)
#endif

// Tracy color definitions (24-bit RGB)
#define STRIGID_COLOR_MEMORY    0xFF6B6B  // Red
#define STRIGID_COLOR_RENDERING 0x4ECDC4  // Cyan
#define STRIGID_COLOR_PHYSICS   0xFFC933  // Yellow
#define STRIGID_COLOR_LOGIC     0x9527F5  // Purple
#define STRIGID_COLOR_NETWORK   0xF38181  // Pink
#define STRIGID_COLOR_AUDIO     0x6600FF  // Purple