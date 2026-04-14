#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <sstream>
#include <chrono>
#include <iomanip>

// Log severity levels
enum class LogLevel
{
	Trace   = 0,
	Debug   = 1,
	Info    = 2,
	Warning = 3,
	Error   = 4,
	Fatal   = 5,

	Always = 0xFF
};

// Log channels — Engine for internal subsystems, Game for user/game-layer code.
// Set per-channel min levels via Logger::SetMinLevel(LogChannel, LogLevel) so
// game developers can silence engine internals without losing their own logs.
enum class LogChannel : uint8_t
{
	Engine = 0,
	Game   = 1,

	Count = 2
};

/// Ring buffer entry for editor log panel consumption.
struct LogEntry
{
	LogLevel Level     = LogLevel::Debug;
	LogChannel Channel = LogChannel::Engine;
	char Message[256];
};

// Singleton logger with thread-safe file writing
class Logger
{
public:
	static Logger& Get()
	{
		static Logger Instance;
		return Instance;
	}

	// Initialize logger with file output. Sets both channel min levels to inMinLevel.
	void Init(const std::string& logFilePath = "TrinyxEngine.log", LogLevel inMinLevel = LogLevel::Debug);

	// Shut down and flush file
	void Shutdown();

	// Set minimum log level for all channels
	void SetMinLevel(LogLevel level)
	{
		for (auto& ml : MinLevel) ml = level;
	}

	// Set minimum log level for a specific channel
	void SetMinLevel(LogChannel channel, LogLevel level)
	{
		MinLevel[static_cast<uint8_t>(channel)] = level;
	}

	// Core logging function
	void Log(LogLevel level, LogChannel channel, const char* file, int line, const std::string& message);

	// --- Editor log ring buffer ---
	static constexpr uint32_t LogRingSize = 1024;

	// Returns pointer to ring buffer and current write head.
	// Reader should snapshot Head, then read entries [Head-LogRingSize .. Head-1] (mod LogRingSize).
	const LogEntry* GetLogRing() const { return LogRing; }
	uint32_t GetLogHead() const { return LogHead; }

private:
	Logger() = default;
	~Logger() { Shutdown(); }
	Logger(const Logger&)            = delete;
	Logger& operator=(const Logger&) = delete;

	std::string GetTimestamp();
	std::string LevelToString(LogLevel level);
	std::string LevelToColor(LogLevel level);

private:
	std::ofstream LogFile;
	std::mutex Mutex;
	LogLevel MinLevel[static_cast<uint8_t>(LogChannel::Count)] = {LogLevel::Debug, LogLevel::Debug};
	bool bInitialized                                          = false;

	// Ring buffer for editor consumption (written under Mutex)
	LogEntry LogRing[LogRingSize]{};
	uint32_t LogHead = 0;
};

// ---------- Debug break macros ----------

// Platform-specific breakpoint intrinsic
#if defined(_MSC_VER)
#define TNX_BREAKPOINT() __debugbreak()
#elif defined(__clang__) || defined(__GNUC__)
#if defined(__x86_64__) || defined(__i386__)
#define TNX_BREAKPOINT() __asm__ volatile("int3")
#elif defined(__aarch64__)
#define TNX_BREAKPOINT() __asm__ volatile("brk #0xF000")
#else
#define TNX_BREAKPOINT() __builtin_trap()
#endif
#else
#define TNX_BREAKPOINT() __builtin_trap()
#endif

// TNX_DEBUG_BREAK() — breakpoint in debug builds, no-op in release.
// Safe to scatter through code as development aids.
#ifdef NDEBUG
#define TNX_DEBUG_BREAK() ((void)0)
#else
#define TNX_DEBUG_BREAK() TNX_BREAKPOINT()
#endif

// TNX_FATAL_BREAK() — always fires. Breakpoint if debugger attached, crash if not.
// Use for invariant violations that must never be silently ignored.
#define TNX_FATAL_BREAK() TNX_BREAKPOINT()

// ---------- Engine channel macros (LOG_ENG_*) ----------
// Used by all engine-internal subsystems. Filter via EngineLogLevel in *.ini.

#define LOG_ENG_TRACE(msg)  Logger::Get().Log(LogLevel::Trace,   LogChannel::Engine, __FILE__, __LINE__, msg)
#define LOG_ENG_DEBUG(msg)  Logger::Get().Log(LogLevel::Debug,   LogChannel::Engine, __FILE__, __LINE__, msg)
#define LOG_ENG_INFO(msg)   Logger::Get().Log(LogLevel::Info,    LogChannel::Engine, __FILE__, __LINE__, msg)
#define LOG_ENG_WARN(msg)   Logger::Get().Log(LogLevel::Warning, LogChannel::Engine, __FILE__, __LINE__, msg)
#define LOG_ENG_ERROR(msg)  Logger::Get().Log(LogLevel::Error,   LogChannel::Engine, __FILE__, __LINE__, msg)
#define LOG_ENG_FATAL(msg)  Logger::Get().Log(LogLevel::Fatal,   LogChannel::Engine, __FILE__, __LINE__, msg)
#define LOG_ENG_ALWAYS(msg) Logger::Get().Log(LogLevel::Always,  LogChannel::Engine, __FILE__, __LINE__, msg)

#define LOG_ENG_TRACE_F(fmt, ...) do { \
    char StrigLogBuff[512]; \
    snprintf(StrigLogBuff, sizeof(StrigLogBuff), fmt, __VA_ARGS__); \
    LOG_ENG_TRACE(StrigLogBuff); \
} while(0)

#define LOG_ENG_DEBUG_F(fmt, ...) do { \
    char StrigLogBuff[512]; \
    snprintf(StrigLogBuff, sizeof(StrigLogBuff), fmt, __VA_ARGS__); \
    LOG_ENG_DEBUG(StrigLogBuff); \
} while(0)

#define LOG_ENG_INFO_F(fmt, ...) do { \
    char StrigLogBuff[512]; \
    snprintf(StrigLogBuff, sizeof(StrigLogBuff), fmt, __VA_ARGS__); \
    LOG_ENG_INFO(StrigLogBuff); \
} while(0)

#define LOG_ENG_WARN_F(fmt, ...) do { \
    char StrigLogBuff[512]; \
    snprintf(StrigLogBuff, sizeof(StrigLogBuff), fmt, __VA_ARGS__); \
    LOG_ENG_WARN(StrigLogBuff); \
} while(0)

#define LOG_ENG_ERROR_F(fmt, ...) do { \
    char StrigLogBuff[512]; \
    snprintf(StrigLogBuff, sizeof(StrigLogBuff), fmt, __VA_ARGS__); \
    LOG_ENG_ERROR(StrigLogBuff); \
    TNX_DEBUG_BREAK(); \
} while(0)

#define LOG_ENG_FATAL_F(fmt, ...) do { \
    char StrigLogBuff[512]; \
    snprintf(StrigLogBuff, sizeof(StrigLogBuff), fmt, __VA_ARGS__); \
    LOG_ENG_FATAL(StrigLogBuff); \
    TNX_FATAL_BREAK(); \
} while(0)

#define LOG_ENG_ALWAYS_F(fmt, ...) do { \
    char StrigLogBuff[512]; \
    snprintf(StrigLogBuff, sizeof(StrigLogBuff), fmt, __VA_ARGS__); \
    LOG_ENG_ALWAYS(StrigLogBuff); \
} while(0)

// ---------- Game channel macros (LOG_*) ----------
// Used by game-layer code: GameMode, GameState, Constructs, custom systems.
// Filter independently via GameLogLevel in *.ini.

#define LOG_TRACE(msg) Logger::Get().Log(LogLevel::Trace,   LogChannel::Game, __FILE__, __LINE__, msg)
#define LOG_DEBUG(msg) Logger::Get().Log(LogLevel::Debug,   LogChannel::Game, __FILE__, __LINE__, msg)
#define LOG_INFO(msg)  Logger::Get().Log(LogLevel::Info,    LogChannel::Game, __FILE__, __LINE__, msg)
#define LOG_WARN(msg)  Logger::Get().Log(LogLevel::Warning, LogChannel::Game, __FILE__, __LINE__, msg)
#define LOG_ERROR(msg) Logger::Get().Log(LogLevel::Error,   LogChannel::Game, __FILE__, __LINE__, msg)
#define LOG_FATAL(msg) Logger::Get().Log(LogLevel::Fatal,   LogChannel::Game, __FILE__, __LINE__, msg)

#define LOG_TRACE_F(fmt, ...) do { \
    char StrigLogBuff[512]; \
    snprintf(StrigLogBuff, sizeof(StrigLogBuff), fmt, __VA_ARGS__); \
    LOG_TRACE(StrigLogBuff); \
} while(0)

#define LOG_DEBUG_F(fmt, ...) do { \
    char StrigLogBuff[512]; \
    snprintf(StrigLogBuff, sizeof(StrigLogBuff), fmt, __VA_ARGS__); \
    LOG_DEBUG(StrigLogBuff); \
} while(0)

#define LOG_INFO_F(fmt, ...) do { \
    char StrigLogBuff[512]; \
    snprintf(StrigLogBuff, sizeof(StrigLogBuff), fmt, __VA_ARGS__); \
    LOG_INFO(StrigLogBuff); \
} while(0)

#define LOG_WARN_F(fmt, ...) do { \
    char StrigLogBuff[512]; \
    snprintf(StrigLogBuff, sizeof(StrigLogBuff), fmt, __VA_ARGS__); \
    LOG_WARN(StrigLogBuff); \
} while(0)

#define LOG_ERROR_F(fmt, ...) do { \
    char StrigLogBuff[512]; \
    snprintf(StrigLogBuff, sizeof(StrigLogBuff), fmt, __VA_ARGS__); \
    LOG_ERROR(StrigLogBuff); \
    TNX_DEBUG_BREAK(); \
} while(0)

#define LOG_FATAL_F(fmt, ...) do { \
    char StrigLogBuff[512]; \
    snprintf(StrigLogBuff, sizeof(StrigLogBuff), fmt, __VA_ARGS__); \
    LOG_FATAL(StrigLogBuff); \
    TNX_FATAL_BREAK(); \
} while(0)
