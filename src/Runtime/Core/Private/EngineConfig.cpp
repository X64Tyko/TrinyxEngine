#include "EngineConfig.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static std::string Trim(const std::string& s)
{
	size_t start = s.find_first_not_of(" \t\r\n");
	size_t end   = s.find_last_not_of(" \t\r\n");
	return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

// Parse "Trace"/"Debug"/"Info"/"Warning"/"Warn"/"Error"/"Fatal" → int value matching LogLevel.
// Returns -1 (Unset) on unrecognised input.
static int ParseLogLevel(const std::string& val)
{
	if (val == "Trace") return 0;
	if (val == "Debug") return 1;
	if (val == "Info") return 2;
	if (val == "Warning" || val == "Warn") return 3;
	if (val == "Error") return 4;
	if (val == "Fatal") return 5;
	// Accept raw integers too
	try { return std::stoi(val); }
	catch (...)
	{
	}
	return -1;
}

static void WriteDefaults(const char* path)
{
	std::ofstream out(path);
	if (!out.is_open()) return;

	out << "[Engine]\n"
		<< "TargetFPS=0\n"
		<< "FixedUpdateHz=128\n"
		<< "NetworkUpdateHz=30\n"
		<< "InputPollHz=1000\n"
		<< "InputNetHz=128\n"
		<< "InputDelayFrames=0\n"
		<< "MaxRenderableEntities=11000\n"
		<< "MaxCachedEntities=25000\n"
		<< "MaxJoltBodies=11000\n"
		<< "TemporalFrameCount=32\n"
		<< "JobCacheSize=16384\n"
		<< "PhysicsUpdateInterval=8\n"
		<< "DefaultScene=\n"
		<< "DefaultState=\n";
}

// Fill fields from an INI file. Only writes fields that are still at sentinel (Unset / empty).
static void FillFromFile(const char* path, EngineConfig& cfg)
{
	std::ifstream file(path);
	if (!file.is_open()) return;

	std::string line;
	while (std::getline(file, line))
	{
		line = Trim(line);
		if (line.empty() || line[0] == '#' || line[0] == '[') continue;

		// Strip inline comments
		size_t comment = line.find('#');
		if (comment != std::string::npos) line = line.substr(0, comment);

		size_t eq = line.find('=');
		if (eq == std::string::npos) continue;

		std::string key = Trim(line.substr(0, eq));
		std::string val = Trim(line.substr(eq + 1));
		if (val.empty()) continue;

		// Only fill if the field is still unset
		if (key == "TargetFPS" && cfg.TargetFPS == EngineConfig::Unset) cfg.TargetFPS = std::stoi(val);
		else if (key == "FixedUpdateHz" && cfg.FixedUpdateHz == EngineConfig::Unset) cfg.FixedUpdateHz = std::stoi(val);
		else if (key == "NetworkUpdateHz" && cfg.NetworkUpdateHz == EngineConfig::Unset) cfg.NetworkUpdateHz = std::stoi(val);
		else if (key == "InputPollHz" && cfg.InputPollHz == EngineConfig::Unset) cfg.InputPollHz = std::stoi(val);
		else if (key == "InputNetHz" && cfg.InputNetHz == EngineConfig::Unset) cfg.InputNetHz = std::stoi(val);
		else if (key == "InputDelayFrames" && cfg.InputDelayFrames == 0) cfg.InputDelayFrames = std::stoi(val);
		else if (key == "MaxRenderableEntities" && cfg.MAX_RENDERABLE_ENTITIES == EngineConfig::Unset) cfg.MAX_RENDERABLE_ENTITIES = std::stoi(val);
		else if (key == "MaxCachedEntities" && cfg.MAX_CACHED_ENTITIES == EngineConfig::Unset) cfg.MAX_CACHED_ENTITIES = std::stoi(val);
		else if (key == "MaxJoltBodies" && cfg.MAX_JOLT_BODIES == EngineConfig::Unset) cfg.MAX_JOLT_BODIES = std::stoi(val);
		else if (key == "TemporalFrameCount" && cfg.TemporalFrameCount == EngineConfig::Unset) cfg.TemporalFrameCount = std::stoi(val);
		else if (key == "JobCacheSize" && cfg.JobCacheSize == EngineConfig::Unset) cfg.JobCacheSize = std::stoi(val);
		else if (key == "PhysicsUpdateInterval" && cfg.PhysicsUpdateInterval == EngineConfig::Unset) cfg.PhysicsUpdateInterval = std::stoi(val);
		else if (key == "DefaultScene" && cfg.DefaultScene[0] == '\0') snprintf(cfg.DefaultScene, sizeof(cfg.DefaultScene), "%s", val.c_str());
		else if (key == "DefaultState" && cfg.DefaultState[0] == '\0') snprintf(cfg.DefaultState, sizeof(cfg.DefaultState), "%s", val.c_str());
		else if (key == "EngineLogLevel" && cfg.EngineLogLevel == EngineConfig::Unset) cfg.EngineLogLevel = ParseLogLevel(val);
		else if (key == "GameLogLevel" && cfg.GameLogLevel == EngineConfig::Unset) cfg.GameLogLevel = ParseLogLevel(val);
		else if (key == "AudioUpdateHz" && cfg.AudioUpdateHz == EngineConfig::Unset) cfg.AudioUpdateHz = std::stoi(val);
		else if (key == "MaxAudioVoices" && cfg.MaxAudioVoices == EngineConfig::Unset) cfg.MaxAudioVoices = std::stoi(val);
	}
}

// Walk up from startDir looking for TrinyxDefaults.ini. Returns the directory it was found in, or empty.
static std::filesystem::path FindEngineRoot(const std::filesystem::path& startDir)
{
	namespace fs = std::filesystem;

	fs::path dir = fs::absolute(startDir);
	for (int depth = 0; depth < 10; ++depth)
	{
		fs::path candidate = dir / "TrinyxDefaults.ini";
		if (fs::exists(candidate)) return dir;

		fs::path parent = dir.parent_path();
		if (parent == dir) break; // hit filesystem root
		dir = parent;
	}
	return {};
}

// Derive project name from the last component of the directory path.
static std::string DeriveProjectName(const char* projectDir)
{
	namespace fs = std::filesystem;
	fs::path p(projectDir);
	// Remove trailing separator if present
	if (p.has_filename()) return p.filename().string();
	return p.parent_path().filename().string();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void EngineConfig::ApplyDefaults()
{
	if (TargetFPS == Unset) TargetFPS = 0;
	if (FixedUpdateHz == Unset) FixedUpdateHz = 128;
	if (PhysicsUpdateInterval == Unset) PhysicsUpdateInterval = 8;
	if (NetworkUpdateHz == Unset) NetworkUpdateHz = 30;
	if (InputPollHz == Unset) InputPollHz = 1000;
	if (InputNetHz == Unset) InputNetHz = 128;
	if (MAX_RENDERABLE_ENTITIES == Unset) MAX_RENDERABLE_ENTITIES = 11000;
	if (MAX_CACHED_ENTITIES == Unset) MAX_CACHED_ENTITIES = 25000;
	if (MAX_JOLT_BODIES == Unset) MAX_JOLT_BODIES = 11000;
	if (TemporalFrameCount == Unset) TemporalFrameCount = 32;
	if (JobCacheSize == Unset) JobCacheSize = 16 * 1024;
	if (AudioUpdateHz == Unset) AudioUpdateHz = 250;
	if (MaxAudioVoices == Unset) MaxAudioVoices = 64;
}

void EngineConfig::FillFrom(const EngineConfig& other)
{
	if (TargetFPS == Unset && other.TargetFPS != Unset) TargetFPS = other.TargetFPS;
	if (FixedUpdateHz == Unset && other.FixedUpdateHz != Unset) FixedUpdateHz = other.FixedUpdateHz;
	if (PhysicsUpdateInterval == Unset && other.PhysicsUpdateInterval != Unset) PhysicsUpdateInterval = other.PhysicsUpdateInterval;
	if (NetworkUpdateHz == Unset && other.NetworkUpdateHz != Unset) NetworkUpdateHz = other.NetworkUpdateHz;
	if (InputPollHz == Unset && other.InputPollHz != Unset) InputPollHz = other.InputPollHz;
	if (InputNetHz == Unset && other.InputNetHz != Unset) InputNetHz = other.InputNetHz;
	if (MAX_RENDERABLE_ENTITIES == Unset && other.MAX_RENDERABLE_ENTITIES != Unset) MAX_RENDERABLE_ENTITIES = other.MAX_RENDERABLE_ENTITIES;
	if (MAX_CACHED_ENTITIES == Unset && other.MAX_CACHED_ENTITIES != Unset) MAX_CACHED_ENTITIES = other.MAX_CACHED_ENTITIES;
	if (MAX_JOLT_BODIES == Unset && other.MAX_JOLT_BODIES != Unset) MAX_JOLT_BODIES = other.MAX_JOLT_BODIES;
	if (TemporalFrameCount == Unset && other.TemporalFrameCount != Unset) TemporalFrameCount = other.TemporalFrameCount;
	if (JobCacheSize == Unset && other.JobCacheSize != Unset) JobCacheSize = other.JobCacheSize;

	if (DefaultScene[0] == '\0' && other.DefaultScene[0] != '\0') snprintf(DefaultScene, sizeof(DefaultScene), "%s", other.DefaultScene);
	if (DefaultState[0] == '\0' && other.DefaultState[0] != '\0') snprintf(DefaultState, sizeof(DefaultState), "%s", other.DefaultState);
	if (AudioUpdateHz == Unset && other.AudioUpdateHz != Unset) AudioUpdateHz = other.AudioUpdateHz;
	if (MaxAudioVoices == Unset && other.MaxAudioVoices != Unset) MaxAudioVoices = other.MaxAudioVoices;
}

EngineConfig EngineConfig::LoadProjectConfig(const char* projectDir)
{
	namespace fs = std::filesystem;

	EngineConfig cfg;

	if (projectDir && projectDir[0] != '\0')
	{
		// 1. Most specific: {ProjectName}Defaults.ini in the project directory
		std::string projectName = DeriveProjectName(projectDir);
		fs::path projectIni     = fs::path(projectDir) / (projectName + "Defaults.ini");
		if (fs::exists(projectIni))
		{
			FillFromFile(projectIni.string().c_str(), cfg);
		}

		// 2. Less specific: TrinyxDefaults.ini — walk up from project dir
		fs::path engineRoot = FindEngineRoot(projectDir);
		if (!engineRoot.empty())
		{
			fs::path engineIni = engineRoot / "TrinyxDefaults.ini";
			FillFromFile(engineIni.string().c_str(), cfg);
		}
	}
	else
	{
		// No project dir — try TrinyxDefaults.ini in CWD
		FillFromFile("TrinyxDefaults.ini", cfg);
	}

	// 3. Compiled-in defaults fill anything still unset
	cfg.ApplyDefaults();
	return cfg;
}

EngineConfig EngineConfig::LoadEditorConfig(const char* projectDir, const EngineConfig& gameConfig)
{
	namespace fs = std::filesystem;

	EngineConfig cfg;

	if (projectDir && projectDir[0] != '\0')
	{
		// 1. Most specific: EditorDefaults.ini in the project directory
		fs::path projectEditorIni = fs::path(projectDir) / "EditorDefaults.ini";
		if (fs::exists(projectEditorIni))
		{
			FillFromFile(projectEditorIni.string().c_str(), cfg);
		}

		// 2. Less specific: EditorDefaults.ini at engine root
		fs::path engineRoot = FindEngineRoot(projectDir);
		if (!engineRoot.empty())
		{
			fs::path engineEditorIni = engineRoot / "EditorDefaults.ini";
			FillFromFile(engineEditorIni.string().c_str(), cfg);
		}
	}

	// 3. Game config fills all remaining gaps
	cfg.FillFrom(gameConfig);

	// 4. Compiled-in defaults (shouldn't be needed, but safety net)
	cfg.ApplyDefaults();
	return cfg;
}

EngineConfig EngineConfig::LoadFromFile(const char* path)
{
	EngineConfig cfg;

	std::ifstream probe(path);
	if (!probe.is_open())
	{
		WriteDefaults(path);
		cfg.ApplyDefaults();
		return cfg;
	}
	probe.close();

	FillFromFile(path, cfg);
	cfg.ApplyDefaults();
	return cfg;
}
