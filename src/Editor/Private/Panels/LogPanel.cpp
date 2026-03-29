#include "Panels/LogPanel.h"
#include "EditorState.h"
#include "Logger.h"
#include "imgui.h"

static ImVec4 LevelColor(LogLevel level)
{
	switch (level)
	{
		case LogLevel::Trace: return {0.6f, 0.6f, 0.6f, 1.0f};
		case LogLevel::Debug: return {0.4f, 0.8f, 0.9f, 1.0f};
		case LogLevel::Info: return {0.4f, 0.9f, 0.4f, 1.0f};
		case LogLevel::Warning: return {1.0f, 0.9f, 0.3f, 1.0f};
		case LogLevel::Error: return {1.0f, 0.3f, 0.3f, 1.0f};
		case LogLevel::Fatal: return {1.0f, 0.2f, 0.8f, 1.0f};
		default: return {1.0f, 1.0f, 1.0f, 1.0f};
	}
}

static const char* LevelTag(LogLevel level)
{
	switch (level)
	{
		case LogLevel::Trace: return "[TRACE]";
		case LogLevel::Debug: return "[DEBUG]";
		case LogLevel::Info: return "[INFO] ";
		case LogLevel::Warning: return "[WARN] ";
		case LogLevel::Error: return "[ERROR]";
		case LogLevel::Fatal: return "[FATAL]";
		default: return "[?????]";
	}
}

void LogPanel::Draw(EditorState& /*state*/)
{
	ImGui::Begin(Title, &bVisible);

	// Filter buttons
	static bool showTrace = false, showDebug = true, showInfo  = true;
	static bool showWarn  = true, showError  = true, showFatal = true;

	ImGui::Checkbox("Trace", &showTrace);
	ImGui::SameLine();
	ImGui::Checkbox("Debug", &showDebug);
	ImGui::SameLine();
	ImGui::Checkbox("Info", &showInfo);
	ImGui::SameLine();
	ImGui::Checkbox("Warn", &showWarn);
	ImGui::SameLine();
	ImGui::Checkbox("Error", &showError);
	ImGui::SameLine();
	ImGui::Checkbox("Fatal", &showFatal);

	static bool autoScroll = true;
	ImGui::SameLine();
	ImGui::Checkbox("Auto-scroll", &autoScroll);

	ImGui::Separator();

	// Log content
	ImGui::BeginChild("LogScroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);

	const Logger& logger = Logger::Get();
	const LogEntry* ring = logger.GetLogRing();
	uint32_t head        = logger.GetLogHead();
	uint32_t ringSize    = Logger::LogRingSize;
	uint32_t start       = (head < ringSize) ? 0 : head - ringSize;

	for (uint32_t i = start; i < head; ++i)
	{
		const LogEntry& entry = ring[i % ringSize];

		// Level filter
		bool show = false;
		switch (entry.Level)
		{
			case LogLevel::Trace: show = showTrace;
				break;
			case LogLevel::Debug: show = showDebug;
				break;
			case LogLevel::Info: show = showInfo;
				break;
			case LogLevel::Warning: show = showWarn;
				break;
			case LogLevel::Error: show = showError;
				break;
			case LogLevel::Fatal: show = showFatal;
				break;
			default: show = true;
				break;
		}

		if (!show) continue;

		ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(entry.Level));
		ImGui::TextUnformatted(LevelTag(entry.Level));
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::TextUnformatted(entry.Message);
	}

	if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);

	ImGui::EndChild();
	ImGui::End();
}