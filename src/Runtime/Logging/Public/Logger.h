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

/// Ring buffer entry for editor log panel consumption.
struct LogEntry
{
	LogLevel Level = LogLevel::Debug;
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

	// Initialize logger with file output
	void Init(const std::string& LogFilePath = "TrinyxEngine.log", LogLevel inMinLevel = LogLevel::Debug);

	// Shut down and flush file
	void Shutdown();

	// Set minimum log level filter
	void SetMinLevel(LogLevel Level) { MinLevel = Level; }

	// Core logging function
	void Log(LogLevel Level, const char* File, int Line, const std::string& Message);

	// --- Editor log ring buffer ---
	static constexpr uint32_t kLogRingSize = 1024;

	// Returns pointer to ring buffer and current write head.
	// Reader should snapshot Head, then read entries [Head-kLogRingSize .. Head-1] (mod kLogRingSize).
	const LogEntry* GetLogRing() const { return LogRing; }
	uint32_t GetLogHead() const { return LogHead; }

private:
	Logger() = default;
	~Logger() { Shutdown(); }
	Logger(const Logger&)            = delete;
	Logger& operator=(const Logger&) = delete;

	std::string GetTimestamp();
	std::string LevelToString(LogLevel Level);
	std::string LevelToColor(LogLevel Level);

private:
	std::ofstream LogFile;
	std::mutex Mutex;
	LogLevel MinLevel = LogLevel::Debug;
	bool bInitialized = false;

	// Ring buffer for editor consumption (written under Mutex)
	LogEntry LogRing[kLogRingSize]{};
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

// Convenience macros for logging
#define LOG_TRACE(msg) Logger::Get().Log(LogLevel::Trace, __FILE__, __LINE__, msg)
#define LOG_DEBUG(msg) Logger::Get().Log(LogLevel::Debug, __FILE__, __LINE__, msg)
#define LOG_INFO(msg) Logger::Get().Log(LogLevel::Info, __FILE__, __LINE__, msg)
#define LOG_WARN(msg) Logger::Get().Log(LogLevel::Warning, __FILE__, __LINE__, msg)
#define LOG_ERROR(msg) Logger::Get().Log(LogLevel::Error, __FILE__, __LINE__, msg)
#define LOG_FATAL(msg) Logger::Get().Log(LogLevel::Fatal, __FILE__, __LINE__, msg)
#define LOG_ALWAYS(msg) Logger::Get().Log(LogLevel::Always, __FILE__, __LINE__, msg)

// Formatted logging macros with variadic arguments
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

#define LOG_ALWAYS_F(fmt, ...) do { \
char StrigLogBuff[512]; \
snprintf(StrigLogBuff, sizeof(StrigLogBuff), fmt, __VA_ARGS__); \
LOG_ALWAYS(StrigLogBuff); \
} while(0)
