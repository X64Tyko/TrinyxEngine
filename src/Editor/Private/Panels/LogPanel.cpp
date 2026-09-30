#include "Panels/LogPanel.h"
#include "EditorState.h"
#include "Logger.h"
#include "TnxStyle.h"
#include "TnxWidgets.h"
#include "imgui.h"

static TnxWidgets::ChipStyle LevelChipStyle(LogLevel level)
{
	switch (level)
	{
		case LogLevel::Warning: return TnxWidgets::ChipStyle::Warn;
		case LogLevel::Error: return TnxWidgets::ChipStyle::Bad;
		case LogLevel::Fatal: return TnxWidgets::ChipStyle::Bad;
		default: return TnxWidgets::ChipStyle::Default;
	}
}

static const char* LevelTag(LogLevel level)
{
	switch (level)
	{
		case LogLevel::Trace: return "TRACE";
		case LogLevel::Debug: return "DEBUG";
		case LogLevel::Info: return "INFO";
		case LogLevel::Warning: return "WARN";
		case LogLevel::Error: return "ERROR";
		case LogLevel::Fatal: return "FATAL";
		default: return "?";
	}
}

static ImVec4 LevelTextColor(LogLevel level)
{
	using namespace TnxStyle::Color;
	switch (level)
	{
		case LogLevel::Trace: return FgGhost;
		case LogLevel::Debug: return FgDim;
		case LogLevel::Info: return Fg;
		case LogLevel::Warning: return Warn;
		case LogLevel::Error: return Bad;
		case LogLevel::Fatal: return Bad;
		default: return Fg;
	}
}

void LogPanel::Draw(EditorState& /*state*/)
{
	if (!BeginPadded())
	{
		ImGui::End();
		return;
	}

	static bool showTrace = false, showDebug = true, showInfo = true;
	static bool showWarn = true, showError = true, showFatal = true;
	static bool autoScroll = true;

	TnxWidgets::PanelHeader(nullptr, "Console", [&]
	{
		// Right side: filter toggles + clear
		// (right-aligned content is drawn by the callback, positioned by PanelHeader)
	});

	// Filter row
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3);
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
	ImGui::Checkbox("Trace", &showTrace);
	ImGui::SameLine(0, 6);
	ImGui::Checkbox("Debug", &showDebug);
	ImGui::SameLine(0, 6);
	ImGui::Checkbox("Info", &showInfo);
	ImGui::SameLine(0, 6);
	ImGui::Checkbox("Warn", &showWarn);
	ImGui::SameLine(0, 6);
	ImGui::Checkbox("Error", &showError);
	ImGui::SameLine(0, 6);
	ImGui::Checkbox("Fatal", &showFatal);
	ImGui::SameLine(0, 14);
	ImGui::Checkbox("Auto-scroll", &autoScroll);
	ImGui::Separator();

	ImGui::BeginChild("LogScroll", ImVec2(0, 0), ImGuiChildFlags_None,
		ImGuiWindowFlags_HorizontalScrollbar);

	const Logger& logger = Logger::Get();
	const LogEntry* ring = logger.GetLogRing();
	uint32_t head        = logger.GetLogHead();
	uint32_t ringSize    = Logger::LogRingSize;
	uint32_t start       = (head < ringSize) ? 0 : head - ringSize;

	ImFont* mono = TnxStyle::Font::MonoRegular ? TnxStyle::Font::MonoRegular : ImGui::GetFont();

	for (uint32_t i = start; i < head; ++i)
	{
		const LogEntry& entry = ring[i % ringSize];

		bool show = false;
		switch (entry.Level)
		{
			case LogLevel::Trace: show = showTrace; break;
			case LogLevel::Debug: show = showDebug; break;
			case LogLevel::Info: show = showInfo; break;
			case LogLevel::Warning: show = showWarn; break;
			case LogLevel::Error: show = showError; break;
			case LogLevel::Fatal: show = showFatal; break;
			default: show = true; break;
		}
		if (!show) continue;

		// Level chip badge
		TnxWidgets::Chip(LevelTag(entry.Level), LevelChipStyle(entry.Level));
		ImGui::SameLine(0, 6);

		// Message in mono, colored by level
		ImGui::PushFont(mono);
		ImGui::PushStyleColor(ImGuiCol_Text, LevelTextColor(entry.Level));
		ImGui::TextUnformatted(entry.Message);
		ImGui::PopStyleColor();
		ImGui::PopFont();
	}

	if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
		ImGui::SetScrollHereY(1.0f);

	ImGui::EndChild();
	ImGui::End();
}
