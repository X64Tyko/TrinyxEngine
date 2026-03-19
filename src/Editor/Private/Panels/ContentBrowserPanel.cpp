#include "Panels/ContentBrowserPanel.h"
#include "AssetDatabase.h"
#include "AssetTypes.h"
#include "EditorContext.h"
#include "EditorState.h"
#include "EngineConfig.h"
#include "MeshManager.h"
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
	if (ImGui::Button("Import Mesh...") && state.EditorCtx)
	{
		state.EditorCtx->ShowImportDialog();
	}
	ImGui::SameLine();
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

	ImGui::Columns(4, "AssetColumns", true);
	ImGui::SetColumnWidth(0, 220.0f);
	ImGui::SetColumnWidth(1, 100.0f);
	ImGui::SetColumnWidth(2, 60.0f);
	ImGui::Text("Path");
	ImGui::NextColumn();
	ImGui::Text("Type");
	ImGui::NextColumn();
	ImGui::Text("MeshID");
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

		// Drag source for prefabs
		if (entry.Type == AssetType::Prefab && state.ConfigPtr)
		{
			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
			{
				std::string absPath = std::string(state.ConfigPtr->ProjectDir)
					+ "/content/" + entry.Path;
				ImGui::SetDragDropPayload("PREFAB_PATH", absPath.c_str(), absPath.size() + 1);
				ImGui::Text("Spawn: %s", entry.Path.c_str());
				ImGui::EndDragDropSource();
			}
		}

		// Double-click: load scenes; prefabs will open prefab editor (TODO)
		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
		{
			if (state.EditorCtx && state.ConfigPtr)
			{
				std::string absPath = std::string(state.ConfigPtr->ProjectDir)
					+ "/content/" + entry.Path;

				if (entry.Type == AssetType::Level) state.EditorCtx->LoadScene(absPath);
				// TODO: Prefab double-click → open prefab editor
			}
		}

		ImGui::NextColumn();
		ImGui::Text("%s", AssetTypeName(entry.Type));
		ImGui::NextColumn();

		// MeshID column — show slot index for mesh assets
		if (entry.Type == AssetType::StaticMesh && state.MeshMgrPtr)
		{
			std::string meshName = std::filesystem::path(entry.Path).stem().string();
			uint32_t meshSlot    = state.MeshMgrPtr->FindSlotByName(meshName);
			if (meshSlot != UINT32_MAX) ImGui::Text("%u", meshSlot);
			else ImGui::TextDisabled("--");
		}
		else ImGui::TextDisabled("--");

		ImGui::NextColumn();
		ImGui::Text("%012llX", static_cast<unsigned long long>(entry.UUID >> 8));
		ImGui::NextColumn();
	}

	ImGui::Columns(1);
	ImGui::EndChild();

	ImGui::End();
}