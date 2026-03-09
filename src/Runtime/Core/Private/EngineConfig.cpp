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

static void WriteDefaults(const char* path, const EngineConfig& cfg)
{
	std::ofstream out(path);
	if (!out.is_open()) return;

	out << "[Engine]\n"
		<< "TargetFPS=" << cfg.TargetFPS << "\n"
		<< "FixedUpdateHz=" << cfg.FixedUpdateHz << "\n"
		<< "NetworkUpdateHz=" << cfg.NetworkUpdateHz << "\n"
		<< "InputPollHz=" << cfg.InputPollHz << "\n"
		<< "MaxPhysicsEntities=" << cfg.MAX_PHYSICS_ENTITIES << "\n"
		<< "MaxCachedEntities=" << cfg.MAX_CACHED_ENTITIES << "\n"
		<< "TemporalFrameCount=" << cfg.TemporalFrameCount << "\n";
}

// Apply key=value pairs from a single INI file onto an existing config.
static void ApplyFromFile(const char* path, EngineConfig& cfg)
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

		if (key == "TargetFPS") cfg.TargetFPS = std::stoi(val);
		else if (key == "FixedUpdateHz") cfg.FixedUpdateHz = std::stoi(val);
		else if (key == "NetworkUpdateHz") cfg.NetworkUpdateHz = std::stoi(val);
		else if (key == "InputPollHz") cfg.InputPollHz = std::stoi(val);
		else if (key == "MaxPhysicsEntities") cfg.MAX_PHYSICS_ENTITIES = std::stoi(val);
		else if (key == "MaxCachedEntities") cfg.MAX_CACHED_ENTITIES = std::stoi(val);
		else if (key == "TemporalFrameCount") cfg.TemporalFrameCount = std::stoi(val);
		else if (key == "JobCacheSize") cfg.JobCacheSize = std::stoi(val);
	}
}

EngineConfig EngineConfig::LoadFromFile(const char* path)
{
	EngineConfig cfg;

	std::ifstream probe(path);
	if (!probe.is_open())
	{
		WriteDefaults(path, cfg);
		return cfg;
	}
	probe.close();

	ApplyFromFile(path, cfg);
	return cfg;
}

EngineConfig EngineConfig::LoadFromDirectory(const char* dir)
{
	namespace fs = std::filesystem;

	EngineConfig cfg;

	fs::path dirPath(dir);
	if (dir[0] == '\0' || !fs::exists(dirPath) || !fs::is_directory(dirPath)) return cfg; // empty or invalid dir — return defaults

	// Collect all *Defaults.ini files, sorted for deterministic load order
	const std::string suffix = "Defaults.ini";
	std::vector<fs::path> iniFiles;
	for (const auto& entry : fs::directory_iterator(dirPath))
	{
		if (!entry.is_regular_file()) continue;
		const auto name = entry.path().filename().string();
		if (name.size() >= suffix.size() &&
			name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
		{
			iniFiles.push_back(entry.path());
		}
	}
	std::sort(iniFiles.begin(), iniFiles.end());

	if (iniFiles.empty())
	{
		// No config found — write defaults as EngineDefaults.ini in the project dir
		fs::path defaultPath = dirPath / "EngineDefaults.ini";
		WriteDefaults(defaultPath.string().c_str(), cfg);
		return cfg;
	}

	// Apply each file in order; later values override earlier ones
	for (const auto& iniFile : iniFiles) ApplyFromFile(iniFile.string().c_str(), cfg);

	return cfg;
}
