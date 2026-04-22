#pragma once
#include <algorithm>

#include "Types.h"

struct EngineConfig
{
	// Sentinel value for int fields — means "not yet set by any config file".
	static constexpr int Unset = -1;

	// ---------------------------------------------------------------------------
	// Cascading config loading — most-specific file wins.
	// Each file only fills fields still at sentinel; it never overrides.
	// ---------------------------------------------------------------------------

	// Game config: {ProjectName}Defaults.ini (projectDir) → TrinyxDefaults.ini (engineDir) → compiled defaults.
	// Derives project name from the directory name. Walks up from projectDir to find TrinyxDefaults.ini.
	static EngineConfig LoadProjectConfig(const char* projectDir);

	// Editor config: EditorDefaults.ini (projectDir) → EditorDefaults.ini (engineDir) → game config fills gaps.
	// Call after LoadProjectConfig; pass the game config so it fills any remaining holes.
	static EngineConfig LoadEditorConfig(const char* projectDir, const EngineConfig& gameConfig);

	// Load a single INI file; writes compiled defaults and returns them if the file does not exist.
	static EngineConfig LoadFromFile(const char* path);

	// Fill any Unset int fields and empty string fields from `other`.
	void FillFrom(const EngineConfig& other);

	// Replace remaining Unset/empty fields with compiled-in defaults.
	void ApplyDefaults();

	// Headless mode: no window, no renderer, no GPU. Set via --headless CLI arg,
	// TNX_HEADLESS compile-time define, or implied by TNX_NET_MODEL=Server.
	bool Headless = false;

	// Thread pinning: assign threads to specific CPU cores for reduced scheduling jitter.
	// Disable in PIE/editor builds where multiple worlds oversubscribe available cores.
	// Can be overridden via EnableThreadPinning=false in EditorDefaults.ini.
	bool EnableThreadPinning = true;

	// Exit the main loop after this many sentinel frames. 0 = run indefinitely.
	// Useful for CI: --max-frames 60 runs one second of logic then exits cleanly.
	int MaxFrames = 0;

	// Variadic Update, let the Logic thread run uncapped or limit its updates, cannot be lower than Fixed update if capped.
	int TargetFPS = Unset;

	// Gameplay Logic (Fixed High) - e.g., 60Hz or 128Hz
	int FixedUpdateHz = Unset;

	// Number of fixed updates per Physics update -> at 8 = fixed: 512Hz physics: 64Hz
	int PhysicsUpdateInterval = Unset;

	// Networking (Fixed Low/Med) - e.g., 20Hz or 30Hz
	// This is your "Tick Rate". Lower = Less Bandwidth, Higher = More Precision.
	int NetworkUpdateHz = Unset;

	// Number of RTT probe pings sent during the Synchronizing phase before
	// computing InputLead. Higher = more accurate estimate; lower = faster load.
	// Default: 8. Range: 1-255.
	int ClockSyncProbes = Unset;

	// Input (and window management)
	// This controls how fast your main thread goes, higher = better input latency
	int InputPollHz = Unset;

	// Rate at which the client sends InputFrame packets to the server.
	// Decoupled from NetworkUpdateHz (state corrections) — higher = lower input latency on server.
	// At 512Hz sim / 128Hz input = 4 sim frames per packet.
	// TODO(lockstep): when InputDelayFrames > 0, ensure InputNetHz >= FixedUpdateHz / InputDelayFrames
	int InputNetHz = Unset;

	// Artificial input delay in sim frames for lockstep/deterministic play.
	// 0 = disabled (default). Brain reads input for (currentFrame - InputDelayFrames).
	// TODO(lockstep): when > 0, extend InputBuffer ring depth to InputDelayFrames + 1 slots.
	// TODO(lockstep): gate Brain::AdvanceFrame on receipt of all peer inputs for (currentFrame - InputDelayFrames).
	int InputDelayFrames = 0;

	// Server: how many sim frames the server is allowed to predict ahead of a client's
	// last received input before stalling the sim for that client.
	// Needs to cover one delivery batch + RTT headroom + startup timing offset.
	// At 512Hz sim / 128Hz input = 4 frames/batch; 64 covers ~125ms of jitter/offset.
	// Set to 0 for strict lockstep.
	int MaxClientInputLead = 64;

	// Arena 1 capacity (Render + Dual partitions). Determines the slab boundary
	// between Arena 1 and Arena 2. Must be <= MAX_CACHED_ENTITIES.
	int MAX_RENDERABLE_ENTITIES = Unset;

	// The max number of Dynamic entities in the world at one time (all arenas combined).
	int MAX_CACHED_ENTITIES = Unset;

	// Maximum Jolt physics bodies. Sizes the body interface, temp allocator, and mapping arrays.
	int MAX_JOLT_BODIES = Unset;

	// Number of temporal frames stored in history. Min 8, must be power of 2.
	// At 128Hz: 128 frames = 1 second of history
	// At 512Hz: 128 frames = 0.25 seconds of history
	int TemporalFrameCount = Unset;

	// number of jobs to preallocate the job queues to hold. exceeding this value will assert
	int JobCacheSize = Unset;

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

	// Log channel min levels. Unset → engine defaults (Info for Engine, Debug for Game).
	// Set via EngineLogLevel / GameLogLevel in *.ini.  Values map to LogLevel: 0=Trace … 4=Error.
	int EngineLogLevel = Unset;
	int GameLogLevel   = Unset;

	// Disable GNS Nagle coalescing for all unreliable sends (not just input).
	// Input sends always bypass Nagle regardless of this setting.
	// Default: false (only input bypasses Nagle).
	bool NoNagle = false;

	// GNS per-connection send rate clamp (bytes/sec). Unset = leave GNS default (256KB/s).
	// Raise this significantly for loopback (PIE) or determinism tests.
	// Both Min and Max should typically be set to the same value to pin the rate.
	// TODO: bring this down once we've characterized real-network bandwidth needs.
	// Example: 10 * 1024 * 1024 = 10MB/s for loopback.
	int SendRateMin = Unset;
	int SendRateMax = Unset;

	// Audio — Sentinel update rate for the AudioManager (fade processing, stream refill).
	// Decoupled from InputPollHz: 250Hz gives 4ms fade resolution at negligible CPU cost.
	int AudioUpdateHz = Unset;

	// Maximum simultaneous voices.  Exceeded voices are stolen by priority.
	int MaxAudioVoices = Unset;

	// --- Helpers ---
	// These resolve Unset to compiled defaults for safe use.
	double GetTargetFrameTime() const
	{
		int fps   = (TargetFPS == Unset) ? 0 : TargetFPS;
		int fixed = (FixedUpdateHz == Unset) ? 128 : FixedUpdateHz;
#ifdef TNX_ENABLE_EDITOR
		return 1.0 / std::max(fps, fixed);
#else
		return (fps > 0)
				   ? 1.0 / std::min(fps, fixed)
				   : 0.0;
#endif
	}

	double GetFixedStepTime() const
	{
		int fixed = (FixedUpdateHz == Unset) ? 128 : FixedUpdateHz;
		return 1.0 / fixed;
	}

	double GetNetworkStepTime() const
	{
		int hz = (NetworkUpdateHz == Unset) ? 30 : NetworkUpdateHz;
		return (hz > 0) ? 1.0 / hz : 0.0;
	}

	int GetInputNetHz() const
	{
		return (InputNetHz == Unset) ? 128 : InputNetHz;
	}

	double GetInputNetStepTime() const
	{
		return 1.0 / GetInputNetHz();
	}
};