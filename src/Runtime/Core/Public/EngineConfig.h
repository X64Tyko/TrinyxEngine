#pragma once
#include <algorithm>

#include "Types.h"

/** @addtogroup core
 *  @{
 */

/**
 * @brief Runtime configuration for a TrinyxEngine instance.
 *
 * Loaded from INI files at startup with cascading precedence (most-specific wins):
 * - Game: `{ProjectName}Defaults.ini` → `TrinyxDefaults.ini` → compiled defaults
 * - Editor overlay: `EditorDefaults.ini` → game config fills gaps
 *
 * All integer fields use @ref Unset as a sentinel meaning "not set; use compiled default."
 * Call @c ApplyDefaults() after loading to resolve all sentinels before use.
 */
struct EngineConfig
{
	static constexpr int Unset = -1; ///< Sentinel: field has not been assigned by any config file.

	/**
	 * @brief Load the cascading game config for @p projectDir.
	 *
	 * Walks `{ProjectName}Defaults.ini` → `TrinyxDefaults.ini` → compiled defaults.
	 * Project name is derived from the directory name.
	 */
	static EngineConfig LoadProjectConfig(const char* projectDir);

	/**
	 * @brief Load the editor config overlay on top of @p gameConfig.
	 *
	 * Walks `EditorDefaults.ini` (projectDir) → `EditorDefaults.ini` (engineDir) → @p gameConfig fills gaps.
	 * @param gameConfig Game config to fall back to for fields not set by editor INI files.
	 */
	static EngineConfig LoadEditorConfig(const char* projectDir, const EngineConfig& gameConfig);

	/**
	 * @brief Load a single INI file into a config struct.
	 * @return Compiled defaults if the file does not exist.
	 */
	static EngineConfig LoadFromFile(const char* path);

	/// @brief Copy all @c Unset int fields and empty strings from @p other into this config.
	void FillFrom(const EngineConfig& other);

	/// @brief Replace all remaining @c Unset / empty fields with compiled-in defaults.
	void ApplyDefaults();

	bool Headless            = false; ///< No window, renderer, or GPU. Implied by `TNX_NET_MODEL=Server`.
	bool EnableThreadPinning = true;  ///< Assign threads to specific cores for reduced jitter. Disable in PIE.

	int MaxFrames = 0; ///< Exit after this many Sentinel frames. 0 = run indefinitely (CI: set to ~60).

	int TargetFPS             = Unset; ///< Main-loop frame rate cap. Cannot be lower than FixedUpdateHz if set.
	int FixedUpdateHz         = Unset; ///< Fixed-timestep logic rate (e.g., 128 or 512 Hz).
	int PhysicsUpdateInterval = Unset; ///< Fixed steps per Jolt step. 8 = 64 Hz physics at 512 Hz sim.

	int NetworkUpdateHz = Unset; ///< State-correction send rate ("tick rate"). Lower = less bandwidth.
	int ClockSyncProbes = Unset; ///< RTT probe count during Synchronizing before computing InputLead. Default: 8, range: 1–255.
	int InputPollHz     = Unset; ///< Sentinel input-poll rate; higher = lower input latency.
	int InputNetHz      = Unset; ///< Rate at which the client sends InputFrame packets to the Authority.

	/// @brief Artificial input delay in sim frames for lockstep/deterministic play.
	/// @note 0 = disabled (default). Brain reads input for (currentFrame - InputDelayFrames).
	int InputDelayFrames = 0;

	/// @brief Max sim frames the Authority may predict ahead of a client's last received input.
	/// @note 0 = strict lockstep. Covers one delivery batch + RTT headroom + startup timing offset.
	int MaxClientInputLead = 16;

	int MAX_RENDERABLE_ENTITIES = Unset; ///< Arena 1 capacity (Render + Dual partitions). Must be ≤ MAX_CACHED_ENTITIES.
	int MAX_CACHED_ENTITIES     = Unset; ///< Max dynamic entities across all arenas combined.
	int MAX_JOLT_BODIES         = Unset; ///< Sizes the Jolt body interface, temp allocator, and mapping arrays.

	int TemporalFrameCount = Unset; ///< Temporal ring depth. Min 8, power-of-2. At 512 Hz: 128 frames ≈ 0.25 s of history.
	int JobCacheSize       = Unset; ///< Pre-allocated job queue capacity. Exceeding this value will assert.

	/// @brief Dirty entity count below which GPU slab upload runs inline on the render thread.
	/// @note Below this count, job dispatch + WaitForCounter overhead exceeds the upload work. Default: 256.
	int SlabUploadInlineThreshold = Unset;
	/// @brief Dirty entity count below which a single bundled job uploads all fields sequentially.
	/// @note Above this threshold, one job per field is dispatched for parallel throughput. Default: 4096.
	int SlabUploadSingleJobThreshold = Unset;

	char ProjectDir[512]   = ""; ///< Project root directory (set from TNX_PROJECT_DIR or Initialize() argument).
	char DefaultScene[256] = ""; ///< Scene to load on startup (relative to ProjectDir/content/).
	char DefaultState[256] = ""; ///< Flow state name to load on startup (e.g., "MainMenu").

	uint16_t NetPort     = 27015;       ///< GNS connection port (CLI: --port).
	char NetAddress[128] = "127.0.0.1"; ///< Remote address for client connections (CLI: --client <ip>).

	int EngineLogLevel = Unset; ///< Min log level for the engine channel. 0=Trace … 4=Error.
	int GameLogLevel   = Unset; ///< Min log level for the game channel. 0=Trace … 4=Error.

	/// @brief Disable GNS Nagle coalescing for all unreliable sends.
	/// @note Input sends always bypass Nagle regardless of this setting.
	bool NoNagle = false;

	/// @brief GNS per-connection send rate floor (bytes/sec). Unset = GNS default (256 KB/s).
	/// @note Set both Min and Max to the same value to pin the rate.
	int SendRateMin = Unset;
	int SendRateMax = Unset; ///< GNS per-connection send rate cap (bytes/sec).

	int AudioUpdateHz  = Unset; ///< Sentinel audio update rate (fade processing, stream refill). Default: 250 Hz.
	int MaxAudioVoices = Unset; ///< Maximum simultaneous voices; exceeded voices are stolen by priority.

	// --- Helpers: resolve Unset to compiled defaults ---

	/// @brief Main-loop target frame time in seconds.
	/// @return 0.0 if uncapped; otherwise 1.0 / min(TargetFPS, FixedUpdateHz) in non-editor builds.
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

	/// @brief Fixed simulation step duration in seconds.
	double GetFixedStepTime() const
	{
		int fixed = (FixedUpdateHz == Unset) ? 128 : FixedUpdateHz;
		return 1.0 / fixed;
	}

	/// @brief Network update interval in seconds (time between state-correction sends).
	double GetNetworkStepTime() const
	{
		int hz = (NetworkUpdateHz == Unset) ? 30 : NetworkUpdateHz;
		return (hz > 0) ? 1.0 / hz : 0.0;
	}

	/// @brief Resolved InputNetHz, falling back to 128 if @c Unset.
	int GetInputNetHz() const
	{
		return (InputNetHz == Unset) ? 128 : InputNetHz;
	}

	/// @brief Client input packet interval in seconds.
	double GetInputNetStepTime() const
	{
		return 1.0 / GetInputNetHz();
	}

	/// @brief Dirty entity count below which slab upload runs inline on the render thread (no job dispatch).
	int GetSlabUploadInlineThreshold() const
	{
		return (SlabUploadInlineThreshold == Unset) ? 256 : SlabUploadInlineThreshold;
	}
	/// @brief Dirty entity count below which a single bundled job uploads all fields.
	int GetSlabUploadSingleJobThreshold() const
	{
		return (SlabUploadSingleJobThreshold == Unset) ? 4096 : SlabUploadSingleJobThreshold;
	}
};

/** @} */
