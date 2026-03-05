#include "EngineConfig.h"
#include <fstream>
#include <string>

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
		<< "MaxDynamicEntities=" << cfg.MaxDynamicEntities << "\n"
		<< "TemporalFrameCount=" << cfg.TemporalFrameCount << "\n";
}

EngineConfig EngineConfig::LoadFromFile(const char* path)
{
	EngineConfig cfg;

	std::ifstream file(path);
	if (!file.is_open())
	{
		WriteDefaults(path, cfg);
		return cfg;
	}

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
		else if (key == "MaxDynamicEntities") cfg.MaxDynamicEntities = std::stoi(val);
		else if (key == "TemporalFrameCount") cfg.TemporalFrameCount = std::stoi(val);
	}

	return cfg;
}