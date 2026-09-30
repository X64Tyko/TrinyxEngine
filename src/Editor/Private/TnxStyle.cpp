#include "TnxStyle.h"

#ifdef IMGUI_ENABLE_FREETYPE
#include "imgui_freetype.h" // ImGuiFreeTypeLoaderFlags_LightHinting
#endif

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace TnxStyle::Font
{
ImFont* DisplayRegular  = nullptr;
ImFont* DisplaySemibold = nullptr;
ImFont* DisplayBold     = nullptr;
ImFont* UiRegular       = nullptr;
ImFont* UiSemibold      = nullptr;
ImFont* MonoRegular     = nullptr;
ImFont* MonoBold        = nullptr;
} // namespace TnxStyle::Font

namespace TnxStyle
{

void LoadFonts(const char* projectDir, const char* engineRoot, float dpiScale)
{
	namespace fs = std::filesystem;

	ImGuiIO& io = ImGui::GetIO();
	io.Fonts->Clear();

	// Build candidate base directories in priority order.
	std::vector<std::string> searchPaths;
	if (projectDir && projectDir[0])
		searchPaths.push_back(std::string(projectDir) + "/assets/fonts");
	if (engineRoot && engineRoot[0])
		searchPaths.push_back(std::string(engineRoot) + "/Trinyx/assets/fonts");
	searchPaths.push_back("assets/fonts"); // CWD fallback

	ImFontConfig cfg;
	cfg.OversampleH       = 2;
	cfg.OversampleV       = 2;
	cfg.RasterizerDensity = dpiScale > 1.01f ? dpiScale : 1.0f;

	const float UI_SIZES     = 20.0f * dpiScale;
	const float DISPLAY_SIZE = 23.0f * dpiScale;
	const float MONO_SIZE    = 18.0f * dpiScale;

	auto tryLoad = [&](const char* filename, float size) -> ImFont*
	{
		for (const auto& base : searchPaths)
		{
			std::string path = base + "/" + filename;
			if (!fs::exists(path)) continue;
			ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), size, &cfg);
			if (f) return f;
		}
		fprintf(stderr, "[TnxStyle] Font not found in any search path: %s\n", filename);
		return nullptr;
	};

	// UI default — Manrope Regular (first-added = ImGui default font)
	Font::UiRegular       = tryLoad("Manrope-Regular.ttf", UI_SIZES);
	Font::UiSemibold      = tryLoad("Manrope-SemiBold.ttf", UI_SIZES);
	Font::DisplayRegular  = tryLoad("SpaceGrotesk-Regular.ttf", DISPLAY_SIZE);
	Font::DisplaySemibold = tryLoad("SpaceGrotesk-SemiBold.ttf", DISPLAY_SIZE);
	Font::DisplayBold     = tryLoad("SpaceGrotesk-Bold.ttf", DISPLAY_SIZE + 2.0f * dpiScale);
	Font::MonoRegular     = tryLoad("JetBrainsMono-Regular.ttf", MONO_SIZE);
	Font::MonoBold        = tryLoad("JetBrainsMono-SemiBold.ttf", MONO_SIZE);

	// If nothing loaded (fonts not downloaded yet), add the built-in font as fallback
	// so the editor still renders something readable.
	bool anyLoaded = Font::UiRegular || Font::UiSemibold || Font::DisplayRegular;
	if (!anyLoaded)
	{
		ImFontConfig fallback;
		fallback.SizePixels  = UI_SIZES;
		fallback.OversampleH = 2;
		fallback.OversampleV = 2;
		io.Fonts->AddFontDefault(&fallback);
	}

	// IMGUI_ENABLE_FREETYPE (defined at compile time) activates FreeType automatically.
	// FontLoaderFlags is read at build time; the Vulkan backend builds the atlas via
	// ImGuiBackendFlags_RendererHasTextures — do NOT call io.Fonts->Build() here.
#ifdef IMGUI_ENABLE_FREETYPE
	io.Fonts->FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
#endif
}

void Apply()
{
	using namespace Color;
	ImGuiStyle& s = ImGui::GetStyle();

	// Layout
	s.WindowPadding      = ImVec2(10, 8); // popups, menus, tooltips; panels push their own
	s.FramePadding       = ImVec2(12, 7);
	s.ItemSpacing        = ImVec2(10, 10);
	s.ItemInnerSpacing   = ImVec2(7, 6);
	s.IndentSpacing      = 20.0f;
	s.ScrollbarSize      = 14.0f;
	s.GrabMinSize        = 12.0f;
	s.TabBarOverlineSize = 2.0f;

	// Borders + rounding
	s.WindowBorderSize = 1.0f;
	s.FrameBorderSize  = 1.0f;
	s.PopupBorderSize  = 1.0f;
	s.TabBorderSize    = 0.0f;
	s.ChildBorderSize  = 1.0f;

	s.WindowRounding       = 4.0f;
	s.FrameRounding        = 2.0f;
	s.PopupRounding        = 6.0f;
	s.ScrollbarRounding    = 1.0f;
	s.GrabRounding         = 2.0f;
	s.TabRounding          = 2.0f;
	s.ChildRounding        = 4.0f;
	s.DockingSeparatorSize = 8.0f;

	// Alignment
	s.WindowTitleAlign    = ImVec2(0.0f, 0.5f);
	s.ButtonTextAlign     = ImVec2(0.5f, 0.5f);
	s.SelectableTextAlign = ImVec2(0.0f, 0.5f);

	s.AntiAliasedLines       = true;
	s.AntiAliasedLinesUseTex = true;
	s.AntiAliasedFill        = true;

	ImVec4* c                = s.Colors;
	c[ImGuiCol_Text]         = LINEAR_FROM_SRGB(Fg);
	c[ImGuiCol_TextDisabled] = LINEAR_FROM_SRGB(FgDim);
	c[ImGuiCol_WindowBg]     = LINEAR_FROM_SRGB(BgApp);
	c[ImGuiCol_ChildBg]      = LINEAR_FROM_SRGB(BgPanel);
	c[ImGuiCol_PopupBg]      = LINEAR_FROM_SRGB(BgPanel);
	c[ImGuiCol_Border]       = LINEAR_FROM_SRGB(Border);
	c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

	c[ImGuiCol_FrameBg]        = LINEAR_FROM_SRGB(BgInput);
	c[ImGuiCol_FrameBgHovered] = LINEAR_FROM_SRGB(ImVec4(BgInput.x, BgInput.y, BgInput.z + 0.05f, 1.0f));
	c[ImGuiCol_FrameBgActive]  = LINEAR_FROM_SRGB(PurpleFaint);

	c[ImGuiCol_TitleBg]          = LINEAR_FROM_SRGB(BgDeep);
	c[ImGuiCol_TitleBgActive]    = LINEAR_FROM_SRGB(PurpleFaint);
	c[ImGuiCol_TitleBgCollapsed] = LINEAR_FROM_SRGB(BgDeep);
	c[ImGuiCol_MenuBarBg]        = LINEAR_FROM_SRGB(BgDeep);

	c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_ScrollbarGrab]        = LINEAR_FROM_SRGB(Border);
	c[ImGuiCol_ScrollbarGrabHovered] = LINEAR_FROM_SRGB(BorderStrong);
	c[ImGuiCol_ScrollbarGrabActive]  = LINEAR_FROM_SRGB(PurpleSoft);

	c[ImGuiCol_CheckMark]        = LINEAR_FROM_SRGB(Purple);
	c[ImGuiCol_SliderGrab]       = LINEAR_FROM_SRGB(Purple);
	c[ImGuiCol_SliderGrabActive] = LINEAR_FROM_SRGB(PurpleHot);

	c[ImGuiCol_Button]        = LINEAR_FROM_SRGB(BgElev);
	c[ImGuiCol_ButtonHovered] = LINEAR_FROM_SRGB(PurpleFaint);
	c[ImGuiCol_ButtonActive]  = LINEAR_FROM_SRGB(PurpleSoft);

	c[ImGuiCol_Header]        = LINEAR_FROM_SRGB(PurpleWash);
	c[ImGuiCol_HeaderHovered] = LINEAR_FROM_SRGB(PurpleFaint);
	c[ImGuiCol_HeaderActive]  = LINEAR_FROM_SRGB(PurpleSoft);

	c[ImGuiCol_Separator]        = LINEAR_FROM_SRGB(BorderSoft);
	c[ImGuiCol_SeparatorHovered] = LINEAR_FROM_SRGB(Border);
	c[ImGuiCol_SeparatorActive]  = LINEAR_FROM_SRGB(PurpleSoft);

	c[ImGuiCol_ResizeGrip]        = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_ResizeGripHovered] = LINEAR_FROM_SRGB(PurpleFaint);
	c[ImGuiCol_ResizeGripActive]  = LINEAR_FROM_SRGB(PurpleSoft);

	c[ImGuiCol_Tab]                 = LINEAR_FROM_SRGB(BgDeep);
	c[ImGuiCol_TabHovered]          = LINEAR_FROM_SRGB(BgElev);
	c[ImGuiCol_TabSelected]         = LINEAR_FROM_SRGB(PurpleFaint);
	c[ImGuiCol_TabSelectedOverline] = LINEAR_FROM_SRGB(Purple);
	c[ImGuiCol_TabDimmed]           = LINEAR_FROM_SRGB(BgDeep);
	c[ImGuiCol_TabDimmedSelected]   = LINEAR_FROM_SRGB(BgElev);

	c[ImGuiCol_DockingPreview] = LINEAR_FROM_SRGB(ImVec4(Purple.x, Purple.y, Purple.z, 0.4f));
	c[ImGuiCol_DockingEmptyBg] = LINEAR_FROM_SRGB(BgDeep);

	c[ImGuiCol_PlotLines]            = LINEAR_FROM_SRGB(ThBrain);
	c[ImGuiCol_PlotLinesHovered]     = LINEAR_FROM_SRGB(Yellow);
	c[ImGuiCol_PlotHistogram]        = LINEAR_FROM_SRGB(Purple);
	c[ImGuiCol_PlotHistogramHovered] = LINEAR_FROM_SRGB(PurpleHot);

	c[ImGuiCol_TableHeaderBg]     = LINEAR_FROM_SRGB(BgDeep);
	c[ImGuiCol_TableBorderStrong] = LINEAR_FROM_SRGB(Border);
	c[ImGuiCol_TableBorderLight]  = LINEAR_FROM_SRGB(BorderSoft);
	c[ImGuiCol_TableRowBg]        = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_TableRowBgAlt]     = ImVec4(1, 1, 1, 0.015f);

	c[ImGuiCol_TextSelectedBg] = LINEAR_FROM_SRGB(PurpleSoft);
	c[ImGuiCol_DragDropTarget] = LINEAR_FROM_SRGB(Yellow);
	c[ImGuiCol_NavHighlight]   = LINEAR_FROM_SRGB(Purple);
}

} // namespace TnxStyle
