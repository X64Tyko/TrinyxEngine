#pragma once
#include <algorithm>

#include "NetTypes.h" // EngineMode

struct EngineConfig
{
	// Load from an INI file; writes defaults and returns them if the file does not exist.
	static EngineConfig LoadFromFile(const char* path);

	// Scan a directory for all *Defaults.ini files and load them (alphabetical order).
	// Later files override earlier values. Writes EngineDefaults.ini if none found.
	static EngineConfig LoadFromDirectory(const char* dir);

	// Engine mode — determines which subsystems are initialized.
	// Set via CLI args (--server, --client, --listen) or programmatically.
	EngineMode Mode = EngineMode::Standalone;

	// Variadic Update, let the Logic thread run uncapped or limit its updates, cannot be lower than Fixed update if capped.
	int TargetFPS = 0; // 0 = Uncapped

	// Gameplay Logic (Fixed High) - e.g., 60Hz or 128Hz
	int FixedUpdateHz = 128;

	// Number of fixed updates per Phyics update -> at 8 = fixed: 512Hz physics: 64Hz
	int PhysicsUpdateInterval = 8;

	// Networking (Fixed Low/Med) - e.g., 20Hz or 30Hz
	// This is your "Tick Rate". Lower = Less Bandwidth, Higher = More Precision.
	int NetworkUpdateHz = 30;

	// Input (and window management)
	// This controls how fast your main thread goes, higher = better input latency
	int InputPollHz = 1000;

	// Arena 1 capacity (Render + Dual partitions). Determines the slab boundary
	// between Arena 1 and Arena 2. Must be <= MAX_CACHED_ENTITIES.
	int MAX_RENDERABLE_ENTITIES = 11000;

	// The max number of Dynamic entities in the world at one time (all arenas combined).
	int MAX_CACHED_ENTITIES = 25000;

	// Maximum Jolt physics bodies. Sizes the body interface, temp allocator, and mapping arrays.
	int MAX_JOLT_BODIES = 11000;

	// Number of temporal frames stored in history. Min 8, must be power of 2.
	// At 128Hz: 128 frames = 1 second of history
	// At 512Hz: 128 frames = 0.25 seconds of history
	int TemporalFrameCount = 8;

	// number of jobs to preallocate the job queues to hold. exceeding this value will assert
	int JobCacheSize = 16 * 1024;

	// Project root directory (set from TNX_PROJECT_DIR or Initialize() argument).
	// Used by the editor's Content Browser to locate assets.
	char ProjectDir[512] = "";

	// Editor: default scene to load on startup (relative to ProjectDir/content/).
	char DefaultScene[256] = "";

	// Default flow state to load on startup (registered name, e.g. "MainMenu").
	char DefaultState[256] = "";

	// Networking — set via CLI args (--server, --client <ip>, --port <port>).
	uint16_t NetPort     = 27015;
	char NetAddress[128] = "127.0.0.1";

	// --- Helpers ---
	double GetTargetFrameTime() const
	{
#ifdef TNX_ENABLE_EDITOR
		return 1.0 / std::max(TargetFPS, FixedUpdateHz);
#else
		return (TargetFPS > 0) ?
				   1.0 / std::min(TargetFPS, FixedUpdateHz)
				   : 0.0;
#endif
	}

	double GetFixedStepTime() const
	{
		return 1.0 / FixedUpdateHz;
	}

	double GetNetworkStepTime() const
	{
		return (NetworkUpdateHz > 0) ? 1.0 / NetworkUpdateHz : 0.0;
	}
};