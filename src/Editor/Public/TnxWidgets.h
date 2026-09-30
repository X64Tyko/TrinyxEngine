#pragma once
#if !defined(TNX_ENABLE_EDITOR)
#error "TnxWidgets.h requires TNX_ENABLE_EDITOR"
#endif

#include "imgui.h"
#include <functional>

namespace TnxWidgets
{

// ---------------------------------------------------------------------------
// Tier badge — colored dot + label pill for SoA component tiers
// ---------------------------------------------------------------------------
enum class Tier
{
	Cold,
	Static,
	Volatile,
	Temporal
};
void TierBadge(Tier tier);

// ---------------------------------------------------------------------------
// Chip — generic pill label used for metadata tags
// ---------------------------------------------------------------------------
enum class ChipStyle
{
	Default,
	Purple,
	Yellow,
	Good,
	Warn,
	Bad
};
void Chip(const char* label, ChipStyle style = ChipStyle::Default, bool mono = true);

// ---------------------------------------------------------------------------
// Kbd — keybinding glyph (e.g. "Ctrl+K") with bottom shadow
// ---------------------------------------------------------------------------
void Kbd(const char* label);

// ---------------------------------------------------------------------------
// PanelHeader — 30px elevated bar that titles a dockable panel.
// Call immediately after ImGui::Begin(), before drawing any content.
// The optional `right` callback draws right-aligned content inside the bar.
// ---------------------------------------------------------------------------
void PanelHeader(const char* icon, const char* title,
	std::function<void()> right = {});

// ---------------------------------------------------------------------------
// Field rows — right-aligned label + mono input atom for the Inspector
// ---------------------------------------------------------------------------
bool FieldFloat(const char* label, float* v,
	const char* unit = nullptr, float labelW = 90.f);
bool FieldVec3(const char* label, float v[3], float labelW = 90.f);
bool FieldBool(const char* label, bool* v, float labelW = 90.f);
bool FieldInt(const char* label, int* v,
	const char* unit = nullptr, float labelW = 90.f);

// ---------------------------------------------------------------------------
// Button variants
// ---------------------------------------------------------------------------
bool Button(const char* label, ImVec2 size = { 0, 0 });
bool ButtonPrimary(const char* label, ImVec2 size = { 0, 0 });
bool ButtonYellow(const char* label, ImVec2 size = { 0, 0 });
bool ButtonGhost(const char* label, ImVec2 size = { 0, 0 });
bool ButtonIconGhost(const char* icon);

// ---------------------------------------------------------------------------
// FrameBudgetBar — progress bar with glow, 100% tick, and a note line
// ---------------------------------------------------------------------------
void FrameBudgetBar(const char* label,
	const char* freqHz,
	float usedMs, float budgetMs,
	ImVec4 color,
	const char* note = nullptr);

} // namespace TnxWidgets
