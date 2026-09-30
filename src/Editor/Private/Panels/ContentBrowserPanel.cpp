#include "Panels/ContentBrowserPanel.h"
#include "AssetDatabase.h"
#include "AssetTypes.h"
#include "EditorContext.h"
#include "EditorState.h"
#include "EngineConfig.h"
#include "MeshManager.h"
#include "TnxStyle.h"
#include "TnxWidgets.h"
#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Type helpers (file-scope, no header dependency on ImVec4)
// ---------------------------------------------------------------------------

static ImVec4 TypeColor(AssetType t)
{
	switch (t)
	{
		case AssetType::Mesh: return { 0.24f, 0.52f, 0.90f, 1.0f };
		case AssetType::Texture: return { 0.90f, 0.58f, 0.20f, 1.0f };
		case AssetType::Audio: return { 0.30f, 0.75f, 0.42f, 1.0f };
		case AssetType::Level: return TnxStyle::Color::PurpleHot;
		case AssetType::Prefab: return TnxStyle::Color::Yellow;
		case AssetType::DataAsset: return TnxStyle::Color::FgMuted;
		case AssetType::Material: return { 0.85f, 0.32f, 0.50f, 1.0f };
		case AssetType::Animation: return { 0.20f, 0.85f, 0.85f, 1.0f };
		case AssetType::Skeleton: return { 0.70f, 0.50f, 0.30f, 1.0f };
		default: return TnxStyle::Color::FgGhost;
	}
}

static const char* TypeAbbr(AssetType t)
{
	switch (t)
	{
		case AssetType::Mesh: return "MSH";
		case AssetType::Texture: return "TEX";
		case AssetType::Audio: return "AUD";
		case AssetType::Level: return "LVL";
		case AssetType::Prefab: return "PFB";
		case AssetType::DataAsset: return "DAT";
		case AssetType::Material: return "MAT";
		case AssetType::Animation: return "ANM";
		case AssetType::Skeleton: return "SKL";
		default: return "???";
	}
}

bool ContentBrowserPanel::MatchesFilter(const AssetDatabaseEntry& entry) const
{
	std::string entryDir = fs::path(entry.Path).parent_path().generic_string();
	if (entryDir != CurrentFolder) return false;
	if (TypeFilter != 0 && static_cast<uint8_t>(entry.Type) != TypeFilter) return false;
	if (SearchBuf[0] != '\0' && !std::strstr(entry.Name.GetStr(), SearchBuf)) return false;
	return true;
}

// ---------------------------------------------------------------------------
// Main draw
// ---------------------------------------------------------------------------

void ContentBrowserPanel::Draw(EditorState& state)
{
	if (!BeginPadded())
	{
		ImGui::End();
		return;
	}
	if (state.ConfigPtr && state.ConfigPtr->ProjectDir[0])
	{
		ContentRoot = std::string(state.ConfigPtr->ProjectDir) + "/content";
		SourceRoot  = std::string(state.ConfigPtr->ProjectDir) + "/src";
	}
	else
	{
		ContentRoot.clear();
		SourceRoot.clear();
	}

	if (!state.AssetDB || ContentRoot.empty())
	{
		ImGui::TextDisabled("No asset database (missing project directory?)");
		ImGui::End();
		return;
	}

	DrawToolbar(state);

	// Split: folder tree (left) + asset area (right)
	ImGui::BeginChild("##FolderTree", { 180.0f, 0 }, ImGuiChildFlags_Borders,
		ImGuiWindowFlags_HorizontalScrollbar);
	DrawFolderPane(state);
	ImGui::EndChild();

	ImGui::SameLine(0, 4.0f);

	ImGui::BeginChild("##AssetArea", { 0, 0 }, ImGuiChildFlags_None);
	if (ActiveSection == BrowserSection::Source && !SourceRoot.empty())
		DrawSourcePane(state);
	else if (CurrentViewMode == ViewMode::Icons)
		DrawAssetPane(state);
	else
		DrawAssetList(state);
	ImGui::EndChild();

	// Modals live at the top level (outside child windows)
	DrawCreatePopup(state);
	DrawSourceCreatePopup(state);

	if (bShowRename)
	{
		ImGui::OpenPopup("##CBRename");
		bShowRename = false;
	}
	ImGui::SetNextWindowSize({ 280, 0 }, ImGuiCond_Always);
	if (ImGui::BeginPopupModal("##CBRename", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
	{
		ImGui::PushFont(TnxStyle::Font::UiSemibold);
		ImGui::Text("Rename Asset");
		ImGui::PopFont();
		ImGui::Spacing();

		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		ImGui::SetNextItemWidth(-1);
		bool enter = ImGui::InputText("##CBRenInput", RenameBuf, sizeof(RenameBuf),
			ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::Spacing();

		bool ok = TnxWidgets::ButtonPrimary("OK") || enter;
		ImGui::SameLine();
		bool cancel = TnxWidgets::ButtonGhost("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);

		if (ok && RenameBuf[0] && state.AssetDB)
		{
			state.AssetDB->Rename(RenamingID, RenameBuf);
			ImGui::CloseCurrentPopup();
		}
		if (cancel) ImGui::CloseCurrentPopup();

		ImGui::EndPopup();
	}

	ImGui::End();
}

// ---------------------------------------------------------------------------
// Toolbar + breadcrumb — single elevated bar with controls right-anchored
// ---------------------------------------------------------------------------

void ContentBrowserPanel::DrawToolbar(EditorState& state)
{
	// --- Integrated header bar ---
	const float H     = 40.0f;
	const ImVec2 barP = ImGui::GetCursorScreenPos();
	const ImVec2 winP = ImGui::GetWindowPos();
	const float winW  = ImGui::GetWindowSize().x;
	ImDrawList* dl    = ImGui::GetWindowDrawList();

	// Rect spans the full window width so the bar is edge-to-edge regardless of padding.
	dl->AddRectFilled({ winP.x, barP.y }, { winP.x + winW, barP.y + H },
		ImGui::ColorConvertFloat4ToU32(TnxStyle::Color::BgElev));
	dl->AddLine({ winP.x, barP.y + H }, { winP.x + winW, barP.y + H },
		ImGui::ColorConvertFloat4ToU32(TnxStyle::Color::BorderSoft), 1.0f);

	// Reduced FramePadding so controls sit neatly inside H=40
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
	const float itemH = ImGui::GetFrameHeight();     // font + 2*4 = ~23px
	const float ctrlY = barP.y + (H - itemH) * 0.5f; // vertically centred

	// Compute right-side total width so we can anchor controls to the right
	static const char* TypeNames[] = {
		"All", "Data", "Mesh", "Skeleton", "Material",
		"Texture", "Audio", "Animation", "Level", "Prefab"
	};
	const float padX    = 8.0f;
	const float searchW = 130.0f;
	const float filterW = 94.0f;
	const float gridW   = ImGui::CalcTextSize("Grid").x + padX * 2.0f;
	const float listW   = ImGui::CalcTextSize("List").x + padX * 2.0f;
	const float sliderW = 68.0f;
	const float gap     = 4.0f;

	float rightW = searchW + gap + filterW + gap + gridW + 2.0f + listW;
	if (CurrentViewMode == ViewMode::Icons) rightW += gap + sliderW;

	// Title — anchored to window left
	ImGui::SetCursorScreenPos({ winP.x + 10.0f,
		barP.y + (H - ImGui::GetTextLineHeight()) * 0.5f });
	ImGui::PushFont(TnxStyle::Font::UiSemibold);
	ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);
	ImGui::TextUnformatted("CONTENT BROWSER");
	ImGui::PopStyleColor();
	ImGui::PopFont();

	// Right-side controls — anchored to window right
	ImGui::SetCursorScreenPos({ winP.x + winW - rightW - 10.0f, ctrlY });

	ImGui::SetNextItemWidth(searchW);
	ImGui::InputTextWithHint("##CBSearch", "Search...", SearchBuf, sizeof(SearchBuf));
	ImGui::SameLine(0, gap);

	ImGui::SetNextItemWidth(filterW);
	ImGui::Combo("##CBTypeFilter", &TypeFilter, TypeNames, 10);
	ImGui::SameLine(0, gap);

	// Grid / List toggle
	auto ViewBtn = [](const char* label, bool active) -> bool
	{
		ImVec4 bg = active ? TnxStyle::Color::PurpleSoft : ImVec4(0, 0, 0, 0);
		ImGui::PushStyleColor(ImGuiCol_Button, bg);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, TnxStyle::Color::PurpleFaint);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, TnxStyle::Color::PurpleSoft);
		bool hit = ImGui::Button(label, { 0, 0 });
		ImGui::PopStyleColor(3);
		return hit;
	};

	if (ViewBtn("Grid", CurrentViewMode == ViewMode::Icons)) CurrentViewMode = ViewMode::Icons;
	ImGui::SameLine(0, 2);
	if (ViewBtn("List", CurrentViewMode == ViewMode::List)) CurrentViewMode = ViewMode::List;

	if (CurrentViewMode == ViewMode::Icons)
	{
		ImGui::SameLine(0, gap);
		ImGui::SetNextItemWidth(sliderW);
		ImGui::SliderFloat("##CBThumb", &ThumbnailSize, 40.0f, 120.0f, "%.0f");
	}

	ImGui::PopStyleVar(); // FramePadding

	// Advance cursor past the bar
	ImGui::SetCursorScreenPos({ barP.x, barP.y + H + 1.0f });

	// --- Secondary row: breadcrumb + section-specific actions ---
	ImGui::Spacing();

	ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);

	auto DrawBreadcrumb = [&](const std::string& rootLabel, std::string& folder)
	{
		ImGui::PushFont(TnxStyle::Font::UiSemibold);
		if (ImGui::SmallButton(rootLabel.c_str())) folder.clear();
		ImGui::PopFont();

		if (!folder.empty())
		{
			std::string accum;
			size_t pos = 0;
			while (pos < folder.size())
			{
				size_t end = folder.find('/', pos);
				if (end == std::string::npos) end = folder.size();

				std::string seg = folder.substr(pos, end - pos);
				if (!accum.empty()) accum += '/';
				accum += seg;

				ImGui::SameLine(0, 2);
				ImGui::TextUnformatted(">");
				ImGui::SameLine(0, 2);

				std::string nav = accum;
				ImGui::PushFont(TnxStyle::Font::UiSemibold);
				if (ImGui::SmallButton(seg.c_str())) folder = nav;
				ImGui::PopFont();

				pos = end + 1;
			}
		}
	};

	if (ActiveSection == BrowserSection::Content)
	{
		if (TnxWidgets::ButtonPrimary("Import...") && state.EditorCtx)
			state.EditorCtx->ShowImportDialog();
		ImGui::SameLine(0, 12.0f);
		DrawBreadcrumb("Content", CurrentFolder);
	}
	else
	{
		DrawBreadcrumb("Source", SourceCurrentFolder);
	}

	ImGui::PopStyleColor();
	ImGui::Spacing();
}

// ---------------------------------------------------------------------------
// Folder tree
// ---------------------------------------------------------------------------

void ContentBrowserPanel::DrawFolderPane(EditorState& state)
{
	// --- Content root ---
	{
		const bool contentRootSelected = (ActiveSection == BrowserSection::Content && CurrentFolder.empty());
		ImGuiTreeNodeFlags rootFlags =
			ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanFullWidth | (contentRootSelected ? ImGuiTreeNodeFlags_Selected : 0);

		ImGui::PushStyleColor(ImGuiCol_Header, TnxStyle::Color::PurpleFaint);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, TnxStyle::Color::BgElev);
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, TnxStyle::Color::PurpleSoft);
		bool rootOpen = ImGui::TreeNodeEx("##cbroot", rootFlags, "Content");
		ImGui::PopStyleColor(3);

		if (ImGui::IsItemClicked(0) && !ImGui::IsItemToggledOpen())
		{
			ActiveSection = BrowserSection::Content;
			CurrentFolder.clear();
		}

		if (ImGui::BeginPopupContextItem("##rootFolderCtx"))
		{
			if (ImGui::MenuItem("New Folder..."))
			{
				ActiveSection = BrowserSection::Content;
				CurrentFolder.clear();
				OpenCreatePopup(true);
			}
			if (ImGui::MenuItem("Refresh"))
				state.AssetDB->Reconcile();
			ImGui::EndPopup();
		}

		if (rootOpen)
		{
			try
			{
				std::vector<fs::path> subdirs;
				for (const auto& e : fs::directory_iterator(ContentRoot))
					if (e.is_directory()) subdirs.push_back(e.path());
				std::sort(subdirs.begin(), subdirs.end());
				for (const auto& d : subdirs)
					DrawFolderNode(d, 0);
			}
			catch (...)
			{
			}
			ImGui::TreePop();
		}
	}

	ImGui::Spacing();

	// --- Source root ---
	if (!SourceRoot.empty())
	{
		const bool sourceRootSelected = (ActiveSection == BrowserSection::Source && SourceCurrentFolder.empty());
		ImGuiTreeNodeFlags srcRootFlags =
			ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanFullWidth | (sourceRootSelected ? ImGuiTreeNodeFlags_Selected : 0);

		ImGui::PushStyleColor(ImGuiCol_Header, TnxStyle::Color::PurpleFaint);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, TnxStyle::Color::BgElev);
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, TnxStyle::Color::PurpleSoft);
		bool srcOpen = ImGui::TreeNodeEx("##srcroot", srcRootFlags, "Source");
		ImGui::PopStyleColor(3);

		if (ImGui::IsItemClicked(0) && !ImGui::IsItemToggledOpen())
		{
			ActiveSection = BrowserSection::Source;
			SourceCurrentFolder.clear();
		}

		if (ImGui::BeginPopupContextItem("##srcRootCtx"))
		{
			if (ImGui::MenuItem("New Construct..."))
			{
				ActiveSection = BrowserSection::Source;
				SourceCurrentFolder.clear();
				OpenSourceCreatePopup(true);
			}
			if (ImGui::MenuItem("New Entity..."))
			{
				ActiveSection = BrowserSection::Source;
				SourceCurrentFolder.clear();
				OpenSourceCreatePopup(false);
			}
			ImGui::EndPopup();
		}

		if (srcOpen)
		{
			try
			{
				std::vector<fs::path> subdirs;
				for (const auto& e : fs::directory_iterator(SourceRoot))
					if (e.is_directory()) subdirs.push_back(e.path());
				std::sort(subdirs.begin(), subdirs.end());
				for (const auto& d : subdirs)
					DrawSourceFolderNode(d, 0);
			}
			catch (...)
			{
			}
			ImGui::TreePop();
		}
	}
}

void ContentBrowserPanel::DrawFolderNode(const fs::path& dir, int depth)
{
	std::string rel;
	try
	{
		rel = fs::relative(dir, ContentRoot).generic_string();
	}
	catch (...)
	{
		return;
	}

	const std::string name = dir.filename().generic_string();
	const bool selected    = (ActiveSection == BrowserSection::Content && CurrentFolder == rel);

	bool hasChildren = false;
	try
	{
		for (const auto& e : fs::directory_iterator(dir))
			if (e.is_directory())
			{
				hasChildren = true;
				break;
			}
	}
	catch (...)
	{
	}

	ImGuiTreeNodeFlags flags =
		ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth | (selected ? ImGuiTreeNodeFlags_Selected : 0) | (!hasChildren ? ImGuiTreeNodeFlags_Leaf : 0);

	ImGui::PushID(rel.c_str());
	ImGui::PushStyleColor(ImGuiCol_Header, TnxStyle::Color::PurpleFaint);
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, TnxStyle::Color::BgElev);
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, TnxStyle::Color::PurpleSoft);
	bool open = ImGui::TreeNodeEx("##folderNode", flags, "%s", name.c_str());
	ImGui::PopStyleColor(3);

	if (ImGui::IsItemClicked(0) && !ImGui::IsItemToggledOpen())
	{
		ActiveSection = BrowserSection::Content;
		CurrentFolder = rel;
	}

	if (ImGui::BeginPopupContextItem("##folderCtx"))
	{
		if (ImGui::MenuItem("New Folder Here..."))
		{
			CurrentFolder = rel;
			OpenCreatePopup(true);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Delete Folder"))
		{
			try
			{
				fs::remove_all(dir);
			}
			catch (...)
			{
			}
			// Navigate up if inside the deleted folder
			if (CurrentFolder == rel || CurrentFolder.rfind(rel + "/", 0) == 0)
				CurrentFolder.clear();
		}
		ImGui::EndPopup();
	}

	if (open)
	{
		try
		{
			std::vector<fs::path> subdirs;
			for (const auto& e : fs::directory_iterator(dir))
				if (e.is_directory()) subdirs.push_back(e.path());
			std::sort(subdirs.begin(), subdirs.end());
			for (const auto& d : subdirs)
				DrawFolderNode(d, depth + 1);
		}
		catch (...)
		{
		}
		ImGui::TreePop();
	}

	ImGui::PopID();
}

// ---------------------------------------------------------------------------
// Source section — folder tree + file pane
// ---------------------------------------------------------------------------

void ContentBrowserPanel::DrawSourceFolderNode(const fs::path& dir, int depth)
{
	std::string rel;
	try
	{
		rel = fs::relative(dir, SourceRoot).generic_string();
	}
	catch (...)
	{
		return;
	}

	const std::string name = dir.filename().generic_string();
	const bool selected    = (ActiveSection == BrowserSection::Source && SourceCurrentFolder == rel);

	bool hasChildren = false;
	try
	{
		for (const auto& e : fs::directory_iterator(dir))
			if (e.is_directory())
			{
				hasChildren = true;
				break;
			}
	}
	catch (...)
	{
	}

	ImGuiTreeNodeFlags flags =
		ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth | (selected ? ImGuiTreeNodeFlags_Selected : 0) | (!hasChildren ? ImGuiTreeNodeFlags_Leaf : 0);

	ImGui::PushID(rel.c_str());
	ImGui::PushStyleColor(ImGuiCol_Header, TnxStyle::Color::PurpleFaint);
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, TnxStyle::Color::BgElev);
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, TnxStyle::Color::PurpleSoft);
	bool open = ImGui::TreeNodeEx("##srcNode", flags, "%s", name.c_str());
	ImGui::PopStyleColor(3);

	if (ImGui::IsItemClicked(0) && !ImGui::IsItemToggledOpen())
	{
		ActiveSection       = BrowserSection::Source;
		SourceCurrentFolder = rel;
	}

	if (ImGui::BeginPopupContextItem("##srcFolderCtx"))
	{
		if (ImGui::MenuItem("New Construct..."))
		{
			ActiveSection       = BrowserSection::Source;
			SourceCurrentFolder = rel;
			OpenSourceCreatePopup(true);
		}
		if (ImGui::MenuItem("New Entity..."))
		{
			ActiveSection       = BrowserSection::Source;
			SourceCurrentFolder = rel;
			OpenSourceCreatePopup(false);
		}
		ImGui::EndPopup();
	}

	if (open)
	{
		try
		{
			std::vector<fs::path> subdirs;
			for (const auto& e : fs::directory_iterator(dir))
				if (e.is_directory()) subdirs.push_back(e.path());
			std::sort(subdirs.begin(), subdirs.end());
			for (const auto& d : subdirs)
				DrawSourceFolderNode(d, depth + 1);
		}
		catch (...)
		{
		}
		ImGui::TreePop();
	}

	ImGui::PopID();
}

ContentBrowserPanel::SourceFileKind
ContentBrowserPanel::DetectSourceKind(const fs::path& filePath)
{
	std::ifstream f(filePath, std::ios::binary);
	if (!f.is_open()) return SourceFileKind::Unknown;

	// Read up to 4 KB — enough to find any registration macro at the top of the file.
	char buf[4096] = {};
	f.read(buf, sizeof(buf) - 1);
	const std::string_view src(buf);

	if (src.find("TNX_REGISTER_SCHEMA") != std::string_view::npos)
		return SourceFileKind::Entity;

	if (src.find("public Construct<") != std::string_view::npos)
		return SourceFileKind::Construct;

	if (src.find("TNX_TEMPORAL_FIELDS") != std::string_view::npos || src.find("TNX_VOLATILE_FIELDS") != std::string_view::npos || src.find("TNX_REGISTER_FIELDS") != std::string_view::npos)
		return SourceFileKind::Component;

	return SourceFileKind::Unknown;
}

static ImVec4 SourceKindColor(ContentBrowserPanel::SourceFileKind k)
{
	switch (k)
	{
		case ContentBrowserPanel::SourceFileKind::Construct: return { 0.35f, 1.0f, 0.55f, 1.0f };
		case ContentBrowserPanel::SourceFileKind::Entity: return { 0.35f, 0.70f, 1.0f, 1.0f };
		case ContentBrowserPanel::SourceFileKind::Component: return { 1.0f, 0.72f, 0.30f, 1.0f };
		default: return TnxStyle::Color::FgGhost;
	}
}

static const char* SourceKindLabel(ContentBrowserPanel::SourceFileKind k)
{
	switch (k)
	{
		case ContentBrowserPanel::SourceFileKind::Construct: return "CONSTRUCT";
		case ContentBrowserPanel::SourceFileKind::Entity: return "ENTITY";
		case ContentBrowserPanel::SourceFileKind::Component: return "COMPONENT";
		default: return "HEADER";
	}
}

void ContentBrowserPanel::DrawSourcePane(EditorState& state)
{
	// Invalidate cache when the folder changes.
	if (SourceKindCacheFolder != SourceCurrentFolder)
	{
		SourceKindCache.clear();
		SourceKindCacheFolder = SourceCurrentFolder;
	}

	fs::path dir = fs::path(SourceRoot);
	if (!SourceCurrentFolder.empty()) dir /= SourceCurrentFolder;

	// Collect .h files in the current source folder.
	std::vector<fs::path> headers;
	try
	{
		for (const auto& e : fs::directory_iterator(dir))
			if (e.is_regular_file() && e.path().extension() == ".h")
				headers.push_back(e.path());
	}
	catch (...)
	{
	}
	std::sort(headers.begin(), headers.end());

	if (ImGui::BeginPopupContextWindow("##SrcPaneCtx",
			ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
	{
		DrawSourceBlankContextMenu(state);
		ImGui::EndPopup();
	}

	if (headers.empty())
	{
		ImGui::Spacing();
		ImGui::TextDisabled("  No headers in this folder.");
		ImGui::Spacing();
		ImGui::TextDisabled("  Right-click to create a Construct or Entity.");
		return;
	}

	// List header
	ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
	ImGui::Columns(2, "##srccols", false);
	ImGui::SetColumnWidth(0, 240.0f);
	ImGui::Text("Name");
	ImGui::NextColumn();
	ImGui::Text("Type");
	ImGui::NextColumn();
	ImGui::PopStyleColor();
	ImGui::Separator();

	for (const auto& path : headers)
	{
		const std::string key  = path.generic_string();
		const std::string stem = path.stem().generic_string();

		// Detect kind, using cache.
		auto it = SourceKindCache.find(key);
		if (it == SourceKindCache.end())
		{
			it = SourceKindCache.emplace(key, DetectSourceKind(path)).first;
		}
		const SourceFileKind kind = it->second;

		ImGui::PushID(key.c_str());

		ImGui::PushStyleColor(ImGuiCol_Header, TnxStyle::Color::PurpleFaint);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, TnxStyle::Color::BgElev);
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, TnxStyle::Color::PurpleSoft);
		bool clicked = ImGui::Selectable(stem.c_str(), false,
			ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
		ImGui::PopStyleColor(3);

		if (clicked && ImGui::IsMouseDoubleClicked(0) && state.EditorCtx)
		{
			if (kind == SourceFileKind::Construct)
				state.EditorCtx->OpenConstructEditor(stem.c_str(), key.c_str());
			else if (kind == SourceFileKind::Entity)
				state.EditorCtx->OpenEntityEditor(stem.c_str(), key.c_str());
		}

		ImGui::NextColumn();

		ImGui::PushStyleColor(ImGuiCol_Text, SourceKindColor(kind));
		ImGui::TextUnformatted(SourceKindLabel(kind));
		ImGui::PopStyleColor();
		ImGui::NextColumn();

		ImGui::PopID();
	}

	ImGui::Columns(1);
}

void ContentBrowserPanel::DrawSourceBlankContextMenu(EditorState& state)
{
	if (ImGui::MenuItem("New Construct..."))
		OpenSourceCreatePopup(true);
	if (ImGui::MenuItem("New Entity..."))
		OpenSourceCreatePopup(false);
}

void ContentBrowserPanel::OpenSourceCreatePopup(bool forConstruct)
{
	bShowSourceCreate      = true;
	bSourceCreateConstruct = forConstruct;
	SourceCreateBuf[0]     = '\0';
}

void ContentBrowserPanel::DrawSourceCreatePopup(EditorState& state)
{
	if (bShowSourceCreate)
	{
		ImGui::OpenPopup("##SrcCreate");
		bShowSourceCreate = false;
	}

	ImGui::SetNextWindowSize({ 300, 0 }, ImGuiCond_Always);
	if (!ImGui::BeginPopupModal("##SrcCreate", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
		return;

	const char* heading = bSourceCreateConstruct ? "New Construct" : "New Entity";
	const char* hint    = bSourceCreateConstruct ? "CMyConstruct" : "EMyEntity";

	ImGui::PushFont(TnxStyle::Font::UiSemibold);
	ImGui::TextUnformatted(heading);
	ImGui::PopFont();
	ImGui::Spacing();

	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::SetNextItemWidth(-1);
	bool enter = ImGui::InputTextWithHint("##SrcCreateName", hint,
		SourceCreateBuf, sizeof(SourceCreateBuf),
		ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::Spacing();

	bool ok = TnxWidgets::ButtonPrimary("Create") || enter;
	ImGui::SameLine();
	bool cancel = TnxWidgets::ButtonGhost("Cancel");

	if (ok && SourceCreateBuf[0])
	{
		CommitSourceCreate(state);
		ImGui::CloseCurrentPopup();
	}
	if (cancel) ImGui::CloseCurrentPopup();

	ImGui::EndPopup();
}

void ContentBrowserPanel::CommitSourceCreate(EditorState& state)
{
	fs::path dir = fs::path(SourceRoot);
	if (!SourceCurrentFolder.empty()) dir /= SourceCurrentFolder;

	const std::string name  = SourceCreateBuf;
	const fs::path filePath = dir / (name + ".h");

	// Write stub header.
	std::ofstream f(filePath);
	if (f.is_open())
	{
		if (bSourceCreateConstruct)
		{
			f << "#pragma once\n"
			  << "#include \"Construct.h\"\n"
			  << "#include \"ConstructView.h\"\n"
			  << "\n"
			  << "class " << name << " : public Construct<" << name << ">\n"
			  << "{\n"
			  << "public:\n"
			  << "    // Add ConstructView<TEntity> and Owned<T> members here.\n"
			  << "};\n";
		}
		else
		{
			f << "#pragma once\n"
			  << "// Entity type stub — add component includes and register below.\n"
			  << "// Example:\n"
			  << "// #include \"CTransform.h\"\n"
			  << "// TNX_REGISTER_ENTITY(" << name << ", CTransform);\n";
		}
	}

	// Invalidate kind cache so the pane rescans.
	SourceKindCache.clear();
	SourceKindCacheFolder.clear();

	// Open the appropriate editor.
	if (state.EditorCtx)
	{
		const std::string absPath = filePath.generic_string();
		if (bSourceCreateConstruct)
			state.EditorCtx->OpenConstructEditor(name.c_str(), absPath.c_str());
		else
			state.EditorCtx->OpenEntityEditor(name.c_str(), absPath.c_str());
	}
}

// ---------------------------------------------------------------------------
// Asset pane — icon grid
// ---------------------------------------------------------------------------

void ContentBrowserPanel::DrawAssetPane(EditorState& state)
{
	const auto& entries = state.AssetDB->GetEntries();
	const float padding = 8.0f;
	const float availW  = ImGui::GetContentRegionAvail().x;
	const int columns   = std::max(1, (int)((availW + padding) / (ThumbnailSize + padding)));

	if (ImGui::BeginPopupContextWindow("##AssetGridCtx",
			ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
	{
		DrawBlankContextMenu(state);
		ImGui::EndPopup();
	}

	int col       = 0;
	bool anyDrawn = false;

	for (const auto& entry : entries)
	{
		if (!MatchesFilter(entry)) continue;
		if (col > 0) ImGui::SameLine(0, padding);
		DrawAssetTile(state, entry);
		if (++col == columns)
		{
			col = 0;
			ImGui::Spacing();
		}
		anyDrawn = true;
	}

	if (!anyDrawn)
	{
		ImGui::Spacing();
		ImGui::TextDisabled("  No assets in this folder.");
		ImGui::Spacing();
		ImGui::TextDisabled("  Right-click to create or import.");
	}
}

void ContentBrowserPanel::DrawAssetTile(EditorState& state, const AssetDatabaseEntry& entry)
{
	const bool selected  = (SelectedAsset == entry.ID);
	const ImVec4 typeCol = TypeColor(entry.Type);
	const char* abbr     = TypeAbbr(entry.Type);
	const char* name     = entry.Name.GetStr();
	const float nameH    = 22.0f;

	ImGui::PushID(entry.Path.c_str());

	ImVec2 p     = ImGui::GetCursorScreenPos();
	bool pressed = ImGui::InvisibleButton("##tile", { ThumbnailSize, ThumbnailSize + nameH });
	bool hovered = ImGui::IsItemHovered();

	if (pressed) SelectedAsset = entry.ID;

	ImDrawList* dl = ImGui::GetWindowDrawList();

	// Tile background — desaturated type color
	float shade = hovered ? 0.75f : 0.55f;
	ImVec4 bg   = { typeCol.x * shade, typeCol.y * shade, typeCol.z * shade, 1.0f };
	dl->AddRectFilled(p, { p.x + ThumbnailSize, p.y + ThumbnailSize },
		ImGui::ColorConvertFloat4ToU32(bg), 6.0f);

	// Border
	if (selected)
		dl->AddRect(p, { p.x + ThumbnailSize, p.y + ThumbnailSize },
			ImGui::ColorConvertFloat4ToU32(TnxStyle::Color::PurpleHot),
			6.0f, 0, 2.0f);
	else if (hovered)
		dl->AddRect(p, { p.x + ThumbnailSize, p.y + ThumbnailSize },
			ImGui::ColorConvertFloat4ToU32(TnxStyle::Color::BorderStrong),
			6.0f, 0, 1.0f);

	// Type abbreviation centered in tile
	ImVec2 abbrSz = ImGui::CalcTextSize(abbr);
	dl->AddText({ p.x + (ThumbnailSize - abbrSz.x) * 0.5f,
					p.y + (ThumbnailSize - abbrSz.y) * 0.5f },
		IM_COL32(255, 255, 255, 200), abbr);

	// Name label — truncate to fit tile width (binary search)
	const float maxNameW = ThumbnailSize - 4.0f;
	const char* nameEnd  = name + std::strlen(name);
	{
		int lo = 0, hi = (int)(nameEnd - name);
		while (lo < hi)
		{
			int mid = (lo + hi + 1) / 2;
			if (ImGui::CalcTextSize(name, name + mid).x <= maxNameW)
				lo = mid;
			else
				hi = mid - 1;
		}
		nameEnd = name + lo;
	}
	ImVec2 nameSz = ImGui::CalcTextSize(name, nameEnd);
	dl->AddText(
		{ p.x + (ThumbnailSize - nameSz.x) * 0.5f, p.y + ThumbnailSize + 3.0f },
		ImGui::ColorConvertFloat4ToU32(selected ? TnxStyle::Color::Fg : TnxStyle::Color::FgMuted),
		name, nameEnd);

	// Context menu
	if (ImGui::BeginPopupContextItem("##tileCtx"))
	{
		DrawAssetContextMenu(state, entry);
		ImGui::EndPopup();
	}

	// Drag sources
	if (entry.Type == AssetType::Prefab && state.ConfigPtr)
	{
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
		{
			std::string absPath = ContentRoot + "/" + entry.Path;
			ImGui::SetDragDropPayload("PREFAB_PATH", absPath.c_str(), absPath.size() + 1);
			ImGui::Text("Spawn: %s", name);
			ImGui::EndDragDropSource();
		}
	}
	if (entry.Type == AssetType::Mesh)
	{
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
		{
			uint32_t slot = MeshManager::Get().FindSlotByID(entry.ID);
			ImGui::SetDragDropPayload("MESH_SLOT", &slot, sizeof(slot));
			ImGui::Text("Mesh: %s", name);
			ImGui::EndDragDropSource();
		}
	}

	// Double-click
	if (hovered && ImGui::IsMouseDoubleClicked(0) && state.EditorCtx)
	{
		std::string absPath = ContentRoot + "/" + entry.Path;
		if (entry.Type == AssetType::Level)
			state.EditorCtx->LoadScene(absPath);
		else if (entry.Type == AssetType::Prefab)
			state.EditorCtx->OpenPrefabEditor(absPath);
	}

	ImGui::PopID();
}

// ---------------------------------------------------------------------------
// Asset pane — list view
// ---------------------------------------------------------------------------

void ContentBrowserPanel::DrawAssetList(EditorState& state)
{
	const auto& entries = state.AssetDB->GetEntries();

	if (ImGui::BeginPopupContextWindow("##AssetListCtx",
			ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
	{
		DrawBlankContextMenu(state);
		ImGui::EndPopup();
	}

	ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
	ImGui::Columns(3, "##listcols", false);
	ImGui::SetColumnWidth(0, 220.0f);
	ImGui::SetColumnWidth(1, 90.0f);
	ImGui::Text("Name");
	ImGui::NextColumn();
	ImGui::Text("Type");
	ImGui::NextColumn();
	ImGui::Text("UUID");
	ImGui::NextColumn();
	ImGui::PopStyleColor();
	ImGui::Separator();

	bool anyDrawn = false;
	for (const auto& entry : entries)
	{
		if (!MatchesFilter(entry)) continue;
		DrawAssetRow(state, entry);
		anyDrawn = true;
	}

	if (!anyDrawn)
	{
		ImGui::Columns(1);
		ImGui::Spacing();
		ImGui::TextDisabled("  No assets in this folder.");
	}
	else
	{
		ImGui::Columns(1);
	}
}

void ContentBrowserPanel::DrawAssetRow(EditorState& state, const AssetDatabaseEntry& entry)
{
	const bool selected = (SelectedAsset == entry.ID);

	ImGui::PushID(entry.Path.c_str());
	ImGui::PushStyleColor(ImGuiCol_Header, TnxStyle::Color::PurpleFaint);
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, TnxStyle::Color::BgElev);
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, TnxStyle::Color::PurpleSoft);

	if (ImGui::Selectable(entry.Name.GetStr(), selected, ImGuiSelectableFlags_SpanAllColumns))
		SelectedAsset = entry.ID;

	if (ImGui::BeginPopupContextItem("##rowCtx"))
	{
		DrawAssetContextMenu(state, entry);
		ImGui::EndPopup();
	}

	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && state.EditorCtx)
	{
		std::string absPath = ContentRoot + "/" + entry.Path;
		if (entry.Type == AssetType::Level)
			state.EditorCtx->LoadScene(absPath);
		else if (entry.Type == AssetType::Prefab)
			state.EditorCtx->OpenPrefabEditor(absPath);
	}

	ImGui::PopStyleColor(3);
	ImGui::NextColumn();

	ImGui::PushStyleColor(ImGuiCol_Text, TypeColor(entry.Type));
	ImGui::TextUnformatted(AssetTypeName(entry.Type));
	ImGui::PopStyleColor();
	ImGui::NextColumn();

	ImGui::TextDisabled("%012llX", static_cast<unsigned long long>(entry.ID.GetUUID() >> 8));
	ImGui::NextColumn();

	ImGui::PopID();
}

// ---------------------------------------------------------------------------
// Context menus
// ---------------------------------------------------------------------------

void ContentBrowserPanel::DrawAssetContextMenu(EditorState& state,
	const AssetDatabaseEntry& entry)
{
	if (entry.Type == AssetType::Level && state.EditorCtx)
	{
		if (ImGui::MenuItem("Open Level"))
			state.EditorCtx->LoadScene(ContentRoot + "/" + entry.Path);
		ImGui::Separator();
	}
	if (entry.Type == AssetType::Prefab && state.EditorCtx)
	{
		std::string absPath = ContentRoot + "/" + entry.Path;
		if (ImGui::MenuItem("Edit Prefab"))
			state.EditorCtx->OpenPrefabEditor(absPath);
		ImGui::Separator();
	}

	if (ImGui::MenuItem("Rename..."))
	{
		bShowRename = true;
		RenamingID  = entry.ID;
		std::snprintf(RenameBuf, sizeof(RenameBuf), "%s", entry.Name.GetStr());
	}

	if (ImGui::MenuItem("Delete"))
	{
		state.AssetDB->Remove(entry.ID);
		try
		{
			fs::remove(fs::path(ContentRoot) / entry.Path);
		}
		catch (...)
		{
		}
		if (SelectedAsset == entry.ID) SelectedAsset = {};
	}

	ImGui::Separator();
	ImGui::TextDisabled("UUID: %012llX",
		static_cast<unsigned long long>(entry.ID.GetUUID() >> 8));
}

void ContentBrowserPanel::DrawBlankContextMenu(EditorState& state)
{
	if (ImGui::BeginMenu("New"))
	{
		if (ImGui::MenuItem("Folder..."))
			OpenCreatePopup(true);

		ImGui::Separator();

		if (ImGui::MenuItem("Level  (.tnxscene)"))
			OpenCreatePopup(false, AssetType::Level);
		if (ImGui::MenuItem("Prefab  (.tnxprefab)"))
			OpenCreatePopup(false, AssetType::Prefab);
		if (ImGui::MenuItem("Data Asset  (.json)"))
			OpenCreatePopup(false, AssetType::DataAsset);

		ImGui::Separator();

		ImGui::BeginDisabled();
		ImGui::MenuItem("Material   (import required)");
		ImGui::MenuItem("Texture    (import required)");
		ImGui::MenuItem("Audio      (import required)");
		ImGui::MenuItem("Animation  (import required)");
		ImGui::EndDisabled();

		ImGui::EndMenu();
	}

	ImGui::Separator();

	if (ImGui::MenuItem("Import Mesh...") && state.EditorCtx)
		state.EditorCtx->ShowImportDialog();

	if (ImGui::MenuItem("Refresh"))
		state.AssetDB->Reconcile();
}

// ---------------------------------------------------------------------------
// Create popup
// ---------------------------------------------------------------------------

void ContentBrowserPanel::OpenCreatePopup(bool forFolder, AssetType type)
{
	bShowCreate      = true;
	bCreateIsFolder  = forFolder;
	CreateType       = type;
	CreateNameBuf[0] = '\0';
}

void ContentBrowserPanel::DrawCreatePopup(EditorState& state)
{
	if (bShowCreate)
	{
		ImGui::OpenPopup("##CBCreate");
		bShowCreate = false;
	}

	ImGui::SetNextWindowSize({ 300, 0 }, ImGuiCond_Always);
	if (!ImGui::BeginPopupModal("##CBCreate", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
		return;

	const char* heading =
		bCreateIsFolder ? "New Folder" : CreateType == AssetType::Level   ? "New Level"
									 : CreateType == AssetType::Prefab    ? "New Prefab"
									 : CreateType == AssetType::DataAsset ? "New Data Asset"
																		  : "New File";

	const char* hint =
		bCreateIsFolder ? "folder_name" : CreateType == AssetType::Level ? "my_level"
									  : CreateType == AssetType::Prefab  ? "my_prefab"
																		 : "my_asset";

	ImGui::PushFont(TnxStyle::Font::UiSemibold);
	ImGui::Text("%s", heading);
	ImGui::PopFont();
	ImGui::Spacing();

	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::SetNextItemWidth(-1);
	bool enter = ImGui::InputTextWithHint("##CBCreateName", hint,
		CreateNameBuf, sizeof(CreateNameBuf),
		ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::Spacing();

	bool ok = TnxWidgets::ButtonPrimary("Create") || enter;
	ImGui::SameLine();
	bool cancel = TnxWidgets::ButtonGhost("Cancel");

	if (ok && CreateNameBuf[0])
	{
		CommitCreate(state);
		ImGui::CloseCurrentPopup();
	}
	if (cancel) ImGui::CloseCurrentPopup();

	ImGui::EndPopup();
}

void ContentBrowserPanel::CommitCreate(EditorState& state)
{
	fs::path base = fs::path(ContentRoot);
	if (!CurrentFolder.empty()) base /= CurrentFolder;

	if (bCreateIsFolder)
	{
		try
		{
			fs::create_directories(base / CreateNameBuf);
		}
		catch (...)
		{
		}
		return;
	}

	const char* ext  = "";
	const char* stub = "";
	switch (CreateType)
	{
		case AssetType::Level:
			ext  = ".tnxscene";
			stub = "{\"version\":1,\"entities\":[]}";
			break;
		case AssetType::Prefab:
			ext  = ".tnxprefab";
			stub = "{\"version\":1,\"root\":null}";
			break;
		case AssetType::DataAsset:
			ext  = ".json";
			stub = "{}";
			break;
		default: return;
	}

	fs::path filePath = base / (std::string(CreateNameBuf) + ext);
	std::ofstream f(filePath);
	if (f.is_open())
	{
		f << stub;
		f.close();
		if (state.AssetDB) state.AssetDB->Reconcile();
	}
}
