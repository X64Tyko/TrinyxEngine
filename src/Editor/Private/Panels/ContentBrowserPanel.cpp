#include "Panels/ContentBrowserPanel.h"
#include "AssetDatabase.h"
#include "AssetTypes.h"
#include "EditorContext.h"
#include "EditorState.h"
#include "EngineConfig.h"
#include "imgui.h"

#include <filesystem>

void ContentBrowserPanel::Draw(EditorState& state)
{
	ImGui::Begin(Title, &bVisible);

	if (!state.AssetDB)
	{
		ImGui::TextDisabled("No asset database (missing project directory?)");
		ImGui::End();
		return;
	}

	const auto& entries = state.AssetDB->GetEntries();

	// --- Toolbar ---
	ImGui::Text("Assets: %zu", entries.size());
	ImGui::SameLine();

	// Type filter combo
	const char* typeNames[] = {
		"All", "Data", "Static Mesh", "Skeletal Mesh", "Material",
		"Texture", "Audio", "Animation", "Level", "Prefab"
	};
	ImGui::SetNextItemWidth(120.0f);
	ImGui::Combo("##TypeFilter", &TypeFilter, typeNames, 10);

	ImGui::Separator();

	// --- Asset list ---
	ImGui::BeginChild("AssetList", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);

	ImGui::Columns(3, "AssetColumns", true);
	ImGui::SetColumnWidth(0, 220.0f);
	ImGui::SetColumnWidth(1, 100.0f);
	ImGui::Text("Path");
	ImGui::NextColumn();
	ImGui::Text("Type");
	ImGui::NextColumn();
	ImGui::Text("UUID");
	ImGui::NextColumn();
	ImGui::Separator();

	for (auto& entry : entries)
	{
		// Apply type filter (0 = All, otherwise match AssetType value)
		if (TypeFilter != 0 && static_cast<uint8_t>(entry.Type) != TypeFilter) continue;

		// Selectable row
		bool selected = false;
		ImGui::Selectable(entry.Path.c_str(), &selected, ImGuiSelectableFlags_SpanAllColumns);

		// Double-click: load scenes
		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
		{
			if (entry.Type == AssetType::Level && state.EditorCtx && state.ConfigPtr)
			{
				std::string absPath = std::string(state.ConfigPtr->ProjectDir)
					+ "/content/" + entry.Path;
				state.EditorCtx->LoadScene(absPath);
			}
		}

		ImGui::NextColumn();
		ImGui::Text("%s", AssetTypeName(entry.Type));
		ImGui::NextColumn();
		ImGui::Text("%012llX", static_cast<unsigned long long>(entry.UUID >> 8));
		ImGui::NextColumn();
	}

	ImGui::Columns(1);
	ImGui::EndChild();

	ImGui::End();
}
