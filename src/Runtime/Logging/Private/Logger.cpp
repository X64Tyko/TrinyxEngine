#include "Logger.h"
#include <iostream>
#include <filesystem>

void Logger::Init(const std::string& LogFilePath, LogLevel inMinLevel)
{
	std::lock_guard<std::mutex> lock(Mutex);

	if (bInitialized)
	{
		return;
	}

	MinLevel = inMinLevel;

	// Open log File in append mode
	LogFile.open(LogFilePath, std::ios::out | std::ios::app);

	if (!LogFile.is_open())
	{
		std::cerr << "Failed to open log File: " << LogFilePath << std::endl;
		return;
	}

	bInitialized = true;

	// Write session header
	LogFile << "\n========================================\n";
	LogFile << "TrinyxEngine Log Session Started\n";
	LogFile << "Timestamp: " << GetTimestamp() << "\n";
	LogFile << "========================================\n\n";
	LogFile.flush();

	std::cout << "[Logger] Initialized - Writing to: " << LogFilePath << std::endl;
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

void Logger::Log(LogLevel Level, const char* File, int Line, const std::string& Message)
{
	// Filter by minimum Level
	if (Level < MinLevel)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(Mutex);

	// Extract Filename from path
	std::string Filename = std::filesystem::path(File).filename().string();

	// Format: [Timestamp] [LEVEL] (File:Line) Message
	std::string logEntry = "[" + GetTimestamp() + "] " +
		"[" + LevelToString(Level) + "] " +
		"(" + Filename + ":" + std::to_string(Line) + ") " +
		Message;

	// Console output with color
	std::cout << LevelToColor(Level) << logEntry << "\033[0m" << std::endl;

	// File output (no color codes)
	if (LogFile.is_open())
	{
		LogFile << logEntry << std::endl;
		LogFile.flush();
	}
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

std::string Logger::LevelToString(LogLevel Level)
{
	switch (Level)
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

std::string Logger::LevelToColor(LogLevel Level)
{
	switch (Level)
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