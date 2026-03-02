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
};

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
} while(0)

#define LOG_FATAL_F(fmt, ...) do { \
    char StrigLogBuff[512]; \
    snprintf(StrigLogBuff, sizeof(StrigLogBuff), fmt, __VA_ARGS__); \
    LOG_FATAL(StrigLogBuff); \
} while(0)

#define LOG_ALWAYS_F(fmt, ...) do { \
char StrigLogBuff[512]; \
snprintf(StrigLogBuff, sizeof(StrigLogBuff), fmt, __VA_ARGS__); \
LOG_ALWAYS(StrigLogBuff); \
} while(0)
