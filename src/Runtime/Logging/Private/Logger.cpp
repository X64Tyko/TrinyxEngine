#include "Logger.h"
#include <iostream>
#include <filesystem>

void Logger::Init(const std::string& logFilePath, LogLevel inMinLevel)
{
	std::lock_guard<std::mutex> lock(Mutex);

	if (bInitialized)
	{
		return;
	}

	MinLevel = inMinLevel;

	// Open log file in append mode
	LogFile.open(logFilePath, std::ios::out | std::ios::app);

	if (!LogFile.is_open())
	{
		std::cerr << "Failed to open log file: " << logFilePath << std::endl;
		return;
	}

	bInitialized = true;

	// Write session header
	LogFile << "\n========================================\n";
	LogFile << "TrinyxEngine Log Session Started\n";
	LogFile << "Timestamp: " << GetTimestamp() << "\n";
	LogFile << "========================================\n\n";
	LogFile.flush();

	std::cout << "[Logger] Initialized - Writing to: " << logFilePath << std::endl;
}

void Logger::Shutdown()
{
	std::lock_guard<std::mutex> lock(Mutex);

	if (!bInitialized)
	{
		return;
	}

	if (LogFile.is_open())
	{
		LogFile << "\n========================================\n";
		LogFile << "TrinyxEngine Log Session Ended\n";
		LogFile << "Timestamp: " << GetTimestamp() << "\n";
		LogFile << "========================================\n\n";
		LogFile.flush();
		LogFile.close();
	}

	bInitialized = false;
}

void Logger::Log(LogLevel level, const char* file, int line, const std::string& message)
{
	// Filter by minimum level
	if (level < MinLevel)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(Mutex);

	// Extract filename from path
	std::string filename = std::filesystem::path(file).filename().string();

	// Format: [Timestamp] [LEVEL] (file:line) message
	std::string logEntry = "[" + GetTimestamp() + "] " +
		"[" + LevelToString(level) + "] " +
		"(" + filename + ":" + std::to_string(line) + ") " +
		message;

	// Console output with color
	std::cout << LevelToColor(level) << logEntry << "\033[0m" << std::endl;

	// File output (no color codes)
	if (LogFile.is_open())
	{
		LogFile << logEntry << std::endl;
		LogFile.flush();
	}

	// Ring buffer for editor panel
	LogEntry& entry = LogRing[LogHead % LogRingSize];
	entry.Level     = level;
	snprintf(entry.Message, sizeof(entry.Message), "%s", message.c_str());
	LogHead++;
}

std::string Logger::GetTimestamp()
{
	auto now  = std::chrono::system_clock::now();
	auto time = std::chrono::system_clock::to_time_t(now);
	auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
		now.time_since_epoch()) % 1000;

	std::tm tm;
#ifdef _WIN32
	localtime_s(&tm, &time);
#else
	localtime_r(&time, &tm);
#endif

	std::ostringstream oss;
	oss << std::put_time(&tm, "%H:%M:%S");
	oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
	return oss.str();
}

std::string Logger::LevelToString(LogLevel level)
{
	switch (level)
	{
		case LogLevel::Trace: return "TRACE";
		case LogLevel::Debug: return "DEBUG";
		case LogLevel::Info: return "INFO ";
		case LogLevel::Warning: return "WARN ";
		case LogLevel::Error: return "ERROR";
		case LogLevel::Fatal: return "FATAL";
		default: return "?????";
	}
}

std::string Logger::LevelToColor(LogLevel level)
{
	switch (level)
	{
		case LogLevel::Trace: return "\033[37m";   // White
		case LogLevel::Debug: return "\033[36m";   // Cyan
		case LogLevel::Info: return "\033[32m";    // Green
		case LogLevel::Warning: return "\033[33m"; // Yellow
		case LogLevel::Error: return "\033[31m";   // Red
		case LogLevel::Fatal: return "\033[35m";   // Magenta
		default: return "\033[0m";                 // Reset
	}
}