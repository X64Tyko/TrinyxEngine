#pragma once
#if !defined(TNX_ENABLE_EDITOR)
#error "TnxStyle.h requires TNX_ENABLE_EDITOR"
#endif

#include "imgui.h"
#include <cmath>

namespace TnxStyle
{

/// Per-channel sRGB → linear conversion (IEC 61966-2-1).
inline float SrgbChan(float c) noexcept
{
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

namespace Color
{
constexpr ImVec4 BgViewport{ 0.027f, 0.027f, 0.040f, 1.0f };
constexpr ImVec4 BgDeep{ 0.050f, 0.050f, 0.068f, 1.0f };
constexpr ImVec4 BgApp{ 0.075f, 0.075f, 0.098f, 1.0f };
constexpr ImVec4 BgPanel{ 0.101f, 0.101f, 0.128f, 1.0f };
constexpr ImVec4 BgElev{ 0.138f, 0.138f, 0.167f, 1.0f };
constexpr ImVec4 BgInput{ 0.042f, 0.042f, 0.060f, 1.0f };
constexpr ImVec4 Border{ 0.178f, 0.174f, 0.213f, 1.0f };
constexpr ImVec4 BorderStrong{ 0.300f, 0.295f, 0.347f, 1.0f };
constexpr ImVec4 BorderSoft{ 0.119f, 0.119f, 0.148f, 1.0f };
constexpr ImVec4 Fg{ 0.921f, 0.920f, 0.932f, 1.0f };
constexpr ImVec4 FgMuted{ 0.595f, 0.593f, 0.625f, 1.0f };
constexpr ImVec4 FgDim{ 0.387f, 0.384f, 0.419f, 1.0f };
constexpr ImVec4 FgGhost{ 0.259f, 0.256f, 0.284f, 1.0f };

constexpr ImVec4 ViewportInner = { 0.231f, 0.184f, 0.306f, 1.0f }; // #3B2F4E  oklch(0.22 0.04 295)
constexpr ImVec4 ViewportOuter = { 0.106f, 0.102f, 0.122f, 1.0f }; // #1B1A1F  oklch(0.12 0.008 285)

constexpr ImVec4 Purple{ 0.565f, 0.415f, 0.898f, 1.0f };
constexpr ImVec4 PurpleHot{ 0.666f, 0.495f, 1.000f, 1.0f };
constexpr ImVec4 PurpleSoft{ 0.305f, 0.213f, 0.503f, 1.0f };
constexpr ImVec4 PurpleFaint{ 0.189f, 0.153f, 0.285f, 1.0f };
constexpr ImVec4 PurpleWash{ 0.189f, 0.153f, 0.285f, 0.50f };

constexpr ImVec4 Yellow{ 0.967f, 0.803f, 0.227f, 1.0f };
constexpr ImVec4 YellowHot{ 1.000f, 0.888f, 0.265f, 1.0f };
constexpr ImVec4 YellowSoft{ 0.636f, 0.511f, 0.000f, 1.0f };
constexpr ImVec4 YellowOnYellow{ 0.165f, 0.142f, 0.020f, 1.0f };

constexpr ImVec4 Good{ 0.382f, 0.771f, 0.407f, 1.0f };
constexpr ImVec4 Warn{ 0.976f, 0.677f, 0.147f, 1.0f };
constexpr ImVec4 Bad{ 0.958f, 0.318f, 0.311f, 1.0f };
constexpr ImVec4 Info{ 0.087f, 0.704f, 0.921f, 1.0f };

constexpr ImVec4 TierCold{ 0.262f, 0.653f, 0.764f, 1.0f };
constexpr ImVec4 TierStatic{ 0.691f, 0.646f, 0.478f, 1.0f };
constexpr ImVec4 TierVolatile{ 0.984f, 0.513f, 0.441f, 1.0f };
constexpr ImVec4 TierTemporal{ 0.630f, 0.506f, 0.939f, 1.0f };

constexpr ImVec4 ThSentinel{ 0.000f, 0.821f, 0.857f, 1.0f };
constexpr ImVec4 ThBrain{ 0.894f, 0.577f, 0.966f, 1.0f };
constexpr ImVec4 ThEncoder{ 0.967f, 0.803f, 0.227f, 1.0f };
} // namespace Color

namespace Font
{
extern ImFont* DisplayRegular;
extern ImFont* DisplaySemibold;
extern ImFont* DisplayBold;
extern ImFont* UiRegular;
extern ImFont* UiSemibold;
extern ImFont* MonoRegular;
extern ImFont* MonoBold;
} // namespace Font

/// Apply Trinyx dark theme — colors, rounding, spacing. Call once after ImGui::CreateContext().
/// Does NOT touch WindowRounding or WindowBg.w — caller is responsible for viewport fixups.
void Apply();

/// Load Manrope, Space Grotesk, and JetBrains Mono via FreeType.
/// Search order per file: {projectDir}/assets/fonts → {engineRoot}/Trinyx/assets/fonts → assets/fonts (CWD).
/// Pass nullptr/empty for either path to skip that tier.
/// Falls back to the ImGui built-in font for any file that is missing from all locations.
/// dpiScale: logical→physical pixel ratio from SDL_GetWindowSize / SDL_GetWindowSizeInPixels.
void LoadFonts(const char* projectDir, const char* engineRoot, float dpiScale = 1.0f);

} // namespace TnxStyle

/// Convert a Color:: constant (authored in sRGB) to linear space for ImGui.
/// Alpha is passed through unchanged.  Use this wherever a TnxStyle::Color
/// value is passed to an ImGui style slot or draw call on an sRGB swapchain.
#define LINEAR_FROM_SRGB(c) \
	ImVec4(TnxStyle::SrgbChan((c).x), TnxStyle::SrgbChan((c).y), TnxStyle::SrgbChan((c).z), (c).w)
