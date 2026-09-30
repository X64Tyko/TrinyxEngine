#include "TnxPalette.h"

#include "imgui_internal.h"
#if !defined(TNX_ENABLE_EDITOR)
#error "TnxPalette.cpp requires TNX_ENABLE_EDITOR"
#endif

#include "TnxStyle.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace TnxPalette
{

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

static std::vector<Command> s_Commands;
static bool s_Open = false;

void Register(Command c)
{
	s_Commands.push_back(std::move(c));
}
void Clear()
{
	s_Commands.clear();
}
void Open()
{
	s_Open = true;
}

// ---------------------------------------------------------------------------
// Fuzzy matcher — simple subsequence with sequential bonus scoring
// Returns -1 on no match, >= 0 on match (higher = better).
// ---------------------------------------------------------------------------

static int FuzzyScore(const char* haystack, const char* needle)
{
	if (!needle || !needle[0]) return 100; // empty query matches everything

	int score = 0;
	int hi = 0, ni = 0;
	bool prevMatched = false;

	while (haystack[hi] && needle[ni])
	{
		char hc = static_cast<char>(std::tolower(static_cast<unsigned char>(haystack[hi])));
		char nc = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[ni])));
		if (hc == nc)
		{
			score += 1;
			if (prevMatched) score += 2; // sequential bonus
			if (ni == 0) score += 4;     // first char bonus
			prevMatched = true;
			ni++;
		}
		else
		{
			prevMatched = false;
		}
		hi++;
	}

	return (needle[ni] == '\0') ? score : -1;
}

static int CommandScore(const Command& cmd, const char* query)
{
	int s = FuzzyScore(cmd.title, query);
	if (cmd.subtitle)
	{
		int ss = FuzzyScore(cmd.subtitle, query);
		if (ss > s) s = ss;
	}
	return s;
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

void Draw()
{
	// Open on Ctrl+K
	if (ImGui::IsKeyPressed(ImGuiKey_K, false)
		&& (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)))
	{
		s_Open = true;
	}

	if (!s_Open) return;

	// Dim full-screen backdrop
	ImGuiViewport* vp = ImGui::GetMainViewport();
	ImDrawList* dl    = ImGui::GetForegroundDrawList();
	dl->AddRectFilled(vp->WorkPos,
		ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y),
		IM_COL32(13, 13, 20, 150));

	// Palette window — centered, auto-height
	static char s_Query[128] = {};
	static int s_SelectedIdx = 0;
	static bool s_JustOpened = false;

	if (s_Open && !s_JustOpened)
	{
		s_Query[0]    = '\0';
		s_SelectedIdx = 0;
		s_JustOpened  = true;
	}

	// Build sorted match list
	struct Match
	{
		int score;
		int idx;
	};
	static std::vector<Match> s_Matches;
	s_Matches.clear();

	for (int i = 0; i < static_cast<int>(s_Commands.size()); ++i)
	{
		int sc = CommandScore(s_Commands[static_cast<size_t>(i)], s_Query);
		if (sc >= 0) s_Matches.push_back({ sc, i });
	}
	std::stable_sort(s_Matches.begin(), s_Matches.end(),
		[](const Match& a, const Match& b)
	{
		return a.score > b.score;
	});

	const int matchCount = static_cast<int>(s_Matches.size());
	s_SelectedIdx        = ImClamp(s_SelectedIdx, 0, matchCount > 0 ? matchCount - 1 : 0);

	// Position and size
	const float W = 620.0f;
	ImGui::SetNextWindowSize(ImVec2(W, 0.0f), ImGuiCond_Always);
	ImGui::SetNextWindowPos(
		ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f - W * 0.5f,
			vp->WorkPos.y + vp->WorkSize.y * 0.12f),
		ImGuiCond_Always);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
	ImGui::PushStyleColor(ImGuiCol_Border, TnxStyle::Color::PurpleSoft);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, TnxStyle::Color::BgPanel);

	ImGuiWindowFlags wFlags = ImGuiWindowFlags_NoTitleBar
							  | ImGuiWindowFlags_NoResize
							  | ImGuiWindowFlags_NoCollapse
							  | ImGuiWindowFlags_NoSavedSettings
							  | ImGuiWindowFlags_NoMove;

	bool keepOpen = true;
	if (ImGui::Begin("##TnxPalette", &keepOpen, wFlags))
	{
		// Search input
		ImGui::PushFont(TnxStyle::Font::MonoRegular);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, TnxStyle::Color::BgInput);
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, TnxStyle::Color::BgInput);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));

		ImGui::SetNextItemWidth(-1.0f);
		if (s_JustOpened) ImGui::SetKeyboardFocusHere();
		bool queryChanged = ImGui::InputTextWithHint("##palette_q",
			"find command, entity, asset…", s_Query, IM_ARRAYSIZE(s_Query));
		if (queryChanged) s_SelectedIdx = 0;

		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(2);
		ImGui::PopFont();

		ImGui::Spacing();

		// Keyboard navigation
		if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) s_SelectedIdx = ImMax(0, s_SelectedIdx - 1);
		if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) s_SelectedIdx = ImMin(matchCount - 1, s_SelectedIdx + 1);
		if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && matchCount > 0)
		{
			const Command& cmd = s_Commands[static_cast<size_t>(s_Matches[s_SelectedIdx].idx)];
			if (cmd.exec) cmd.exec();
			s_Open       = false;
			s_JustOpened = false;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			s_Open       = false;
			s_JustOpened = false;
		}

		// Command rows
		constexpr int kMaxVisible = 8;
		int shown                 = 0;
		for (int mi = 0; mi < matchCount && shown < kMaxVisible; ++mi, ++shown)
		{
			const Command& cmd = s_Commands[static_cast<size_t>(s_Matches[mi].idx)];
			bool isSelected    = (mi == s_SelectedIdx);

			// Row background
			ImVec4 rowBg = isSelected ? TnxStyle::Color::PurpleWash : ImVec4(0, 0, 0, 0);
			ImGui::PushStyleColor(ImGuiCol_Header, rowBg);
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, TnxStyle::Color::BgElev);
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, TnxStyle::Color::PurpleWash);

			char rowId[32];
			snprintf(rowId, sizeof(rowId), "##pcmd_%d", mi);
			bool clicked = ImGui::Selectable(rowId, isSelected,
				ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, 36.0f));
			ImGui::PopStyleColor(3);

			if (clicked)
			{
				if (cmd.exec) cmd.exec();
				s_Open       = false;
				s_JustOpened = false;
				break;
			}

			// Overlay text onto the selectable row
			ImVec2 rowMin = ImGui::GetItemRectMin();
			ImVec2 rowMax = ImGui::GetItemRectMax();
			float cy      = (rowMin.y + rowMax.y) * 0.5f;

			// Icon (if any)
			float textX = rowMin.x + 12.0f;

			// Title
			dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
				ImVec2(textX, cy - ImGui::GetTextLineHeight() * 0.5f),
				ImGui::ColorConvertFloat4ToU32(
					isSelected ? TnxStyle::Color::Fg : TnxStyle::Color::FgMuted),
				cmd.title);

			// Subtitle (dimmed, right of title)
			if (cmd.subtitle)
			{
				float titleW = ImGui::CalcTextSize(cmd.title).x;
				dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.88f,
					ImVec2(textX + titleW + 8.0f, cy - ImGui::GetTextLineHeight() * 0.44f),
					ImGui::ColorConvertFloat4ToU32(TnxStyle::Color::FgDim),
					cmd.subtitle);
			}

			// Keybind chip (right-aligned)
			if (cmd.keybind)
			{
				float kbW = ImGui::CalcTextSize(cmd.keybind).x + 12.0f;
				float kbX = rowMax.x - kbW - 8.0f;
				float kbY = cy - ImGui::GetTextLineHeight() * 0.5f;
				dl->AddRectFilled(ImVec2(kbX, kbY - 2.0f), ImVec2(kbX + kbW, kbY + ImGui::GetTextLineHeight() + 2.0f),
					ImGui::ColorConvertFloat4ToU32(TnxStyle::Color::BgElev), 3.0f);
				dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
					ImVec2(kbX + 6.0f, kbY),
					ImGui::ColorConvertFloat4ToU32(TnxStyle::Color::FgDim),
					cmd.keybind);
			}
		}

		if (matchCount == 0)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
			ImGui::TextUnformatted("  No matching commands.");
			ImGui::PopStyleColor();
		}
	}
	ImGui::End();

	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(3);

	if (!keepOpen)
	{
		s_Open       = false;
		s_JustOpened = false;
	}

	s_JustOpened = false;
}

} // namespace TnxPalette
