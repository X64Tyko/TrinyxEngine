#pragma once
#include <algorithm>

struct EngineConfig
{
	// Load from an INI file; writes defaults and returns them if the file does not exist.
	static EngineConfig LoadFromFile(const char* path);

	// Scan a directory for all *Defaults.ini files and load them (alphabetical order).
	// Later files override earlier values. Writes EngineDefaults.ini if none found.
	static EngineConfig LoadFromDirectory(const char* dir);
	// Variadic Update, let the Logic thread run uncapped or limit its updates, cannot be lower than Fixed update if capped.
	int TargetFPS = 0; // 0 = Uncapped

	// Physics/Simulation (Fixed High) - e.g., 60Hz or 128Hz
	int FixedUpdateHz = 128;

	// Networking (Fixed Low/Med) - e.g., 20Hz or 30Hz
	// This is your "Tick Rate". Lower = Less Bandwidth, Higher = More Precision.
	int NetworkUpdateHz = 30;

	// Input (and window management)
	// This controls how fast your main thread goes, higher = better input latency
	int InputPollHz = 1000;

	int MAX_PHYSICS_ENTITIES = 11000;

	// The max number of Dynamic entities in the world at one time.
	int MAX_CACHED_ENTITIES = 25000;

	// Number of temporal frames stored in history. Min 8, must be power of 2.
	// At 128Hz: 128 frames = 1 second of history
	// At 512Hz: 128 frames = 0.25 seconds of history
	int TemporalFrameCount = 8;

	// number of jobs to preallocate the job queues to hold. exceeding this value will assert
	int JobCacheSize = 1024;

	// --- Helpers ---
	double GetTargetFrameTime() const
	{
		return (TargetFPS > 0) ? 1.0 / std::min(TargetFPS, FixedUpdateHz) : 0.0;
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