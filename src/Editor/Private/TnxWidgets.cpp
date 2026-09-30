#include "TnxWidgets.h"
#include "TnxStyle.h"
#include "imgui_internal.h"
#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace
{

void DrawPill(ImVec2 pos, ImVec2 size, ImVec4 bgColor, ImVec4 borderColor, float rounding = 2.0f)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
		ImGui::ColorConvertFloat4ToU32(bgColor), rounding);
	dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
		ImGui::ColorConvertFloat4ToU32(borderColor), rounding, 0, 1.0f);
}

// Right-align text within a fixed column width.
void TextRightAligned(float columnWidth, const char* text)
{
	float textW = ImGui::CalcTextSize(text).x;
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + columnWidth - textW);
	ImGui::TextUnformatted(text);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// TierBadge
// ---------------------------------------------------------------------------
void TnxWidgets::TierBadge(Tier tier)
{
	using namespace TnxStyle::Color;

	const char* label = "";
	ImVec4 color;
	switch (tier)
	{
		case Tier::Cold:
			label = "COLD";
			color = TierCold;
			break;
		case Tier::Static:
			label = "STATIC";
			color = TierStatic;
			break;
		case Tier::Volatile:
			label = "VOLATILE";
			color = TierVolatile;
			break;
		case Tier::Temporal:
			label = "TEMPORAL";
			color = TierTemporal;
			break;
	}

	ImFont* font = TnxStyle::Font::MonoBold ? TnxStyle::Font::MonoBold : ImGui::GetFont();
	ImGui::PushFont(font);

	const float padX = 6.0f, padY = 2.0f, dotR = 2.5f, dotGap = 5.0f;
	const ImVec2 textSize = ImGui::CalcTextSize(label);
	const ImVec2 pos      = ImGui::GetCursorScreenPos();
	const ImVec2 size     = ImVec2(padX * 2 + dotR * 2 + dotGap + textSize.x,
		textSize.y + padY * 2);

	ImVec4 bg  = color;
	bg.w       = 0.18f;
	ImVec4 brd = color;
	brd.w      = 0.55f;
	DrawPill(pos, size, bg, brd);

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddCircleFilled(ImVec2(pos.x + padX + dotR, pos.y + size.y * 0.5f),
		dotR, ImGui::ColorConvertFloat4ToU32(color));
	dl->AddText(ImVec2(pos.x + padX + dotR * 2 + dotGap, pos.y + padY),
		ImGui::ColorConvertFloat4ToU32(color), label);

	ImGui::Dummy(size);
	ImGui::PopFont();
}

// ---------------------------------------------------------------------------
// Chip
// ---------------------------------------------------------------------------
void TnxWidgets::Chip(const char* label, ChipStyle style, bool mono)
{
	using namespace TnxStyle::Color;

	ImFont* font = (mono && TnxStyle::Font::MonoRegular)
					   ? TnxStyle::Font::MonoRegular
					   : ImGui::GetFont();
	ImGui::PushFont(font);

	ImVec4 bg, fg, brd;
	switch (style)
	{
		case ChipStyle::Default:
			bg  = BgElev;
			fg  = FgMuted;
			brd = BorderSoft;
			break;
		case ChipStyle::Purple:
			bg  = PurpleWash;
			fg  = PurpleHot;
			brd = PurpleSoft;
			break;
		case ChipStyle::Yellow:
			bg  = ImVec4(0.35f, 0.27f, 0.02f, 0.40f);
			fg  = Yellow;
			brd = YellowSoft;
			break;
		case ChipStyle::Good:
			bg  = ImVec4(0.10f, 0.30f, 0.15f, 0.40f);
			fg  = Good;
			brd = Good;
			break;
		case ChipStyle::Warn:
			bg  = ImVec4(0.30f, 0.20f, 0.05f, 0.40f);
			fg  = Warn;
			brd = Warn;
			break;
		case ChipStyle::Bad:
			bg  = ImVec4(0.30f, 0.10f, 0.10f, 0.50f);
			fg  = Bad;
			brd = Bad;
			break;
	}

	const float padX = 6.0f, padY = 2.0f;
	const ImVec2 textSize = ImGui::CalcTextSize(label);
	const ImVec2 pos      = ImGui::GetCursorScreenPos();
	const ImVec2 size     = ImVec2(padX * 2 + textSize.x, textSize.y + padY * 2);

	DrawPill(pos, size, bg, brd);
	ImGui::GetWindowDrawList()->AddText(
		ImVec2(pos.x + padX, pos.y + padY),
		ImGui::ColorConvertFloat4ToU32(fg), label);

	ImGui::Dummy(size);
	ImGui::PopFont();
}

// ---------------------------------------------------------------------------
// Kbd
// ---------------------------------------------------------------------------
void TnxWidgets::Kbd(const char* label)
{
	using namespace TnxStyle::Color;

	ImFont* font = TnxStyle::Font::MonoRegular ? TnxStyle::Font::MonoRegular : ImGui::GetFont();
	ImGui::PushFont(font);

	const float padX = 5.0f, padY = 1.0f;
	const ImVec2 textSize = ImGui::CalcTextSize(label);
	const ImVec2 pos      = ImGui::GetCursorScreenPos();
	const ImVec2 size     = ImVec2(ImMax(18.0f, padX * 2 + textSize.x), textSize.y + padY * 2);

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
		ImGui::ColorConvertFloat4ToU32(BgInput), 2.0f);
	// Border on three sides, thicker bottom edge to fake a key shadow
	dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y - 1),
		ImGui::ColorConvertFloat4ToU32(Border), 2.0f);
	dl->AddLine(ImVec2(pos.x, pos.y + size.y - 1),
		ImVec2(pos.x + size.x, pos.y + size.y - 1),
		ImGui::ColorConvertFloat4ToU32(Border), 2.0f);
	dl->AddText(ImVec2(pos.x + (size.x - textSize.x) * 0.5f, pos.y + padY),
		ImGui::ColorConvertFloat4ToU32(FgMuted), label);

	ImGui::Dummy(size);
	ImGui::PopFont();
}

// ---------------------------------------------------------------------------
// PanelHeader
// ---------------------------------------------------------------------------
void TnxWidgets::PanelHeader(const char* icon, const char* title,
	std::function<void()> right)
{
	using namespace TnxStyle::Color;

	const float H       = 36.0f;
	const ImVec2 cursor = ImGui::GetCursorScreenPos();
	const ImVec2 winP   = ImGui::GetWindowPos();
	const float winW    = ImGui::GetWindowSize().x;
	ImDrawList* dl      = ImGui::GetWindowDrawList();

	// Background rect spans the full window width regardless of WindowPadding.
	dl->AddRectFilled({ winP.x, cursor.y }, { winP.x + winW, cursor.y + H },
		ImGui::ColorConvertFloat4ToU32(BgElev));
	dl->AddLine({ winP.x, cursor.y + H }, { winP.x + winW, cursor.y + H },
		ImGui::ColorConvertFloat4ToU32(BorderSoft), 1.0f);

	const float textY = cursor.y + (H - ImGui::GetTextLineHeight()) * 0.5f;
	ImGui::SetCursorScreenPos(ImVec2(winP.x + 10, textY));

	ImGui::PushStyleColor(ImGuiCol_Text, FgMuted);

	if (icon && icon[0] != '\0')
	{
		ImGui::TextUnformatted(icon);
		ImGui::SameLine(0, 6);
	}

	ImFont* semibold = TnxStyle::Font::UiSemibold ? TnxStyle::Font::UiSemibold : ImGui::GetFont();
	ImGui::PushFont(semibold);

	// Upper-case the title
	char upper[64];
	int i = 0;
	for (; title[i] && i < 63; ++i)
		upper[i] = (title[i] >= 'a' && title[i] <= 'z') ? (char)(title[i] - 32) : title[i];
	upper[i] = '\0';
	ImGui::TextUnformatted(upper);

	ImGui::PopFont();
	ImGui::PopStyleColor();

	if (right)
	{
		ImGui::SameLine();
		float rightX = winP.x + winW - 6.0f;
		ImGui::SetCursorScreenPos(ImVec2(rightX, textY));
		right();
	}

	// Advance past the header bar; cursor.x is already at the padded content left.
	ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + H + 1));
}

// ---------------------------------------------------------------------------
// FieldFloat
// ---------------------------------------------------------------------------
bool TnxWidgets::FieldFloat(const char* label, float* v,
	const char* unit, float labelW)
{
	using namespace TnxStyle::Color;
	bool changed = false;

	ImGui::BeginGroup();
	ImGui::AlignTextToFramePadding();
	ImGui::PushStyleColor(ImGuiCol_Text, FgMuted);
	TextRightAligned(labelW, label);
	ImGui::PopStyleColor();
	ImGui::SameLine(0, 6);

	ImFont* mono = TnxStyle::Font::MonoRegular ? TnxStyle::Font::MonoRegular : ImGui::GetFont();
	ImGui::PushFont(mono);

	char id[64];
	snprintf(id, sizeof(id), "##flt_%s", label);
	float avail = ImGui::GetContentRegionAvail().x;
	if (unit) avail -= ImGui::CalcTextSize(unit).x + 6.0f;
	ImGui::SetNextItemWidth(avail);
	changed = ImGui::DragFloat(id, v, 0.01f, 0.f, 0.f, "%.3f");

	if (unit)
	{
		ImGui::SameLine(0, 4);
		ImGui::PushStyleColor(ImGuiCol_Text, FgDim);
		ImGui::TextUnformatted(unit);
		ImGui::PopStyleColor();
	}
	ImGui::PopFont();
	ImGui::EndGroup();
	return changed;
}

// ---------------------------------------------------------------------------
// FieldVec3
// ---------------------------------------------------------------------------
bool TnxWidgets::FieldVec3(const char* label, float v[3], float labelW)
{
	using namespace TnxStyle::Color;
	bool changed = false;

	static const ImVec4 axisColors[3] = {
		{ 0.96f, 0.39f, 0.31f, 1.0f }, // X — red
		{ 0.46f, 0.83f, 0.40f, 1.0f }, // Y — green
		{ 0.35f, 0.62f, 0.95f, 1.0f }, // Z — blue
	};
	static const char* axisLabels[3] = { "X", "Y", "Z" };

	ImGui::BeginGroup();
	ImGui::AlignTextToFramePadding();
	ImGui::PushStyleColor(ImGuiCol_Text, FgMuted);
	TextRightAligned(labelW, label);
	ImGui::PopStyleColor();
	ImGui::SameLine(0, 6);

	ImFont* mono = TnxStyle::Font::MonoRegular ? TnxStyle::Font::MonoRegular : ImGui::GetFont();
	ImGui::PushFont(mono);

	float fieldW = (ImGui::GetContentRegionAvail().x - 2.0f) / 3.0f;

	for (int i = 0; i < 3; ++i)
	{
		if (i > 0) ImGui::SameLine(0, 2);

		ImGui::PushStyleColor(ImGuiCol_Text, axisColors[i]);
		ImGui::TextUnformatted(axisLabels[i]);
		ImGui::PopStyleColor();
		ImGui::SameLine(0, 2);

		char id[32];
		snprintf(id, sizeof(id), "##v3_%s_%d", label, i);
		ImGui::SetNextItemWidth(fieldW - ImGui::CalcTextSize(axisLabels[i]).x - 4.0f);
		if (ImGui::DragFloat(id, &v[i], 0.01f, 0.f, 0.f, "%.3f"))
			changed = true;
	}

	ImGui::PopFont();
	ImGui::EndGroup();
	return changed;
}

// ---------------------------------------------------------------------------
// FieldBool
// ---------------------------------------------------------------------------
bool TnxWidgets::FieldBool(const char* label, bool* v, float labelW)
{
	using namespace TnxStyle::Color;
	bool changed = false;

	ImGui::BeginGroup();
	ImGui::AlignTextToFramePadding();
	ImGui::PushStyleColor(ImGuiCol_Text, FgMuted);
	TextRightAligned(labelW, label);
	ImGui::PopStyleColor();
	ImGui::SameLine(0, 6);

	char id[64];
	snprintf(id, sizeof(id), "##bool_%s", label);
	changed = ImGui::Checkbox(id, v);
	ImGui::EndGroup();
	return changed;
}

// ---------------------------------------------------------------------------
// FieldInt
// ---------------------------------------------------------------------------
bool TnxWidgets::FieldInt(const char* label, int* v,
	const char* unit, float labelW)
{
	using namespace TnxStyle::Color;
	bool changed = false;

	ImGui::BeginGroup();
	ImGui::AlignTextToFramePadding();
	ImGui::PushStyleColor(ImGuiCol_Text, FgMuted);
	TextRightAligned(labelW, label);
	ImGui::PopStyleColor();
	ImGui::SameLine(0, 6);

	ImFont* mono = TnxStyle::Font::MonoRegular ? TnxStyle::Font::MonoRegular : ImGui::GetFont();
	ImGui::PushFont(mono);

	char id[64];
	snprintf(id, sizeof(id), "##int_%s", label);
	float avail = ImGui::GetContentRegionAvail().x;
	if (unit) avail -= ImGui::CalcTextSize(unit).x + 6.0f;
	ImGui::SetNextItemWidth(avail);
	changed = ImGui::DragInt(id, v);

	if (unit)
	{
		ImGui::SameLine(0, 4);
		ImGui::PushStyleColor(ImGuiCol_Text, FgDim);
		ImGui::TextUnformatted(unit);
		ImGui::PopStyleColor();
	}
	ImGui::PopFont();
	ImGui::EndGroup();
	return changed;
}

// ---------------------------------------------------------------------------
// Button variants
// ---------------------------------------------------------------------------
bool TnxWidgets::Button(const char* label, ImVec2 size)
{
	return ImGui::Button(label, size);
}

bool TnxWidgets::ButtonPrimary(const char* label, ImVec2 size)
{
	using namespace TnxStyle::Color;
	ImGui::PushStyleColor(ImGuiCol_Button, Purple);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, PurpleHot);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, PurpleSoft);
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
	ImFont* semi = TnxStyle::Font::UiSemibold ? TnxStyle::Font::UiSemibold : ImGui::GetFont();
	ImGui::PushFont(semi);
	bool clicked = ImGui::Button(label, size);
	ImGui::PopFont();
	ImGui::PopStyleColor(4);
	return clicked;
}

bool TnxWidgets::ButtonYellow(const char* label, ImVec2 size)
{
	using namespace TnxStyle::Color;
	ImGui::PushStyleColor(ImGuiCol_Button, Yellow);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, YellowHot);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, YellowSoft);
	ImGui::PushStyleColor(ImGuiCol_Text, YellowOnYellow);
	ImFont* semi = TnxStyle::Font::UiSemibold ? TnxStyle::Font::UiSemibold : ImGui::GetFont();
	ImGui::PushFont(semi);
	bool clicked = ImGui::Button(label, size);
	ImGui::PopFont();
	ImGui::PopStyleColor(4);
	return clicked;
}

bool TnxWidgets::ButtonGhost(const char* label, ImVec2 size)
{
	using namespace TnxStyle::Color;
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, PurpleFaint);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, PurpleSoft);
	bool clicked = ImGui::Button(label, size);
	ImGui::PopStyleColor(3);
	return clicked;
}

bool TnxWidgets::ButtonIconGhost(const char* icon)
{
	using namespace TnxStyle::Color;
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, PurpleFaint);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, PurpleSoft);
	bool clicked = ImGui::Button(icon, ImVec2(22, 22));
	ImGui::PopStyleColor(3);
	return clicked;
}

// ---------------------------------------------------------------------------
// FrameBudgetBar
// ---------------------------------------------------------------------------
void TnxWidgets::FrameBudgetBar(const char* label, const char* freqHz,
	float usedMs, float budgetMs,
	ImVec4 color, const char* note)
{
	using namespace TnxStyle::Color;
	ImDrawList* dl = ImGui::GetWindowDrawList();

	ImGui::BeginGroup();

	// Row 1: colored dot + label + freq + "X.XX / X.XX ms" right-aligned
	ImVec2 p = ImGui::GetCursorScreenPos();
	dl->AddRectFilled(p, ImVec2(p.x + 8, p.y + 8),
		ImGui::ColorConvertFloat4ToU32(color), 2.0f);
	ImGui::Dummy(ImVec2(8, 8));
	ImGui::SameLine(0, 6);

	ImFont* bold = TnxStyle::Font::MonoBold ? TnxStyle::Font::MonoBold : ImGui::GetFont();
	ImFont* reg  = TnxStyle::Font::MonoRegular ? TnxStyle::Font::MonoRegular : ImGui::GetFont();

	ImGui::PushFont(bold);
	ImGui::TextUnformatted(label);
	ImGui::PopFont();

	ImGui::SameLine(0, 6);
	ImGui::PushFont(reg);
	ImGui::PushStyleColor(ImGuiCol_Text, FgDim);
	ImGui::TextUnformatted(freqHz);
	ImGui::PopStyleColor();

	char rhs[64];
	snprintf(rhs, sizeof(rhs), "%.2f / %.2f ms", usedMs, budgetMs);
	float rhsW = ImGui::CalcTextSize(rhs).x;
	ImGui::SameLine(0, 0);
	float curX  = ImGui::GetCursorPosX();
	float avail = ImGui::GetContentRegionAvail().x;
	ImGui::SetCursorPosX(curX + avail - rhsW);
	ImGui::TextUnformatted(rhs);
	ImGui::PopFont();

	// Row 2: progress bar with glow
	const float W = ImGui::GetContentRegionAvail().x;
	const float H = 8.0f;
	ImVec2 b0     = ImGui::GetCursorScreenPos();
	ImVec2 b1     = ImVec2(b0.x + W, b0.y + H);

	dl->AddRectFilled(b0, b1, ImGui::ColorConvertFloat4ToU32(BgInput), 1.0f);

	float ratio = (budgetMs > 0.f) ? (usedMs / budgetMs) : 0.f;
	float fillW = ImClamp(ratio, 0.0f, 1.5f) * W;
	if (fillW > 0.f)
	{
		bool over     = usedMs > budgetMs;
		ImU32 fillCol = over ? ImGui::ColorConvertFloat4ToU32(Bad)
							 : ImGui::ColorConvertFloat4ToU32(color);
		dl->AddRectFilled(b0, ImVec2(b0.x + ImMin(fillW, W), b1.y), fillCol, 1.0f);

		ImVec4 glow = over ? Bad : color;
		glow.w      = 0.35f;
		dl->AddRectFilled(ImVec2(b0.x, b0.y - 1),
			ImVec2(b0.x + ImMin(fillW, W), b1.y + 1),
			ImGui::ColorConvertFloat4ToU32(glow), 1.0f);
	}
	// 100% tick mark at the right edge
	dl->AddLine(ImVec2(b1.x, b0.y - 2), ImVec2(b1.x, b1.y + 2),
		ImGui::ColorConvertFloat4ToU32(FgDim), 1.0f);

	ImGui::Dummy(ImVec2(W, H + 2));

	// Row 3: optional note
	if (note && note[0] != '\0')
	{
		ImGui::PushStyleColor(ImGuiCol_Text, FgDim);
		ImGui::TextUnformatted(note);
		ImGui::PopStyleColor();
	}

	ImGui::EndGroup();
}
