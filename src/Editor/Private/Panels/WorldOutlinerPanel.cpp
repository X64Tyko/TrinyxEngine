#include "Panels/WorldOutlinerPanel.h"
#include "ConstructRegistry.h"
#include "EditorContext.h"
#include "EditorState.h"
#include "LogicThreadBase.h"
#include "Registry.h"
#include "TnxStyle.h"
#include "TnxWidgets.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <cstring>

// Strip MSVC "class "/"struct " prefixes from typeid names.
static const char* StripTypePrefix(const char* name)
{
	if (!name) return "(unknown)";
	if (std::strncmp(name, "class ", 6) == 0) return name + 6;
	if (std::strncmp(name, "struct ", 7) == 0) return name + 7;
	return name;
}

void WorldOutlinerPanel::Draw(EditorState& state)
{
	if (!BeginPadded())
	{
		ImGui::End();
		return;
	}

	ConstructRegistry* cr = state.LogicPtr ? state.LogicPtr->GetConstructRegistry() : nullptr;

	uint32_t constructCount = cr ? cr->GetCount() : 0;
	uint32_t archetypeCount = state.RegistryPtr
								  ? static_cast<uint32_t>(state.RegistryPtr->GetArchetypes().size())
								  : 0;
	uint32_t entityCount    = state.RegistryPtr
								  ? state.RegistryPtr->GetTotalEntityCount()
								  : 0;

	TnxWidgets::PanelHeader(nullptr, "World Outliner", [&]()
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%u cnst", constructCount);
		TnxWidgets::Chip(buf);
		ImGui::SameLine(0.0f, 4.0f);
		snprintf(buf, sizeof(buf), "%u arch", archetypeCount);
		TnxWidgets::Chip(buf);
		ImGui::SameLine(0.0f, 4.0f);
		if (entityCount >= 1000)
			snprintf(buf, sizeof(buf), "%uk ent", entityCount / 1000);
		else
			snprintf(buf, sizeof(buf), "%u ent", entityCount);
		TnxWidgets::Chip(buf);
	});

	ImGui::BeginChild("##OutlinerBody", ImVec2(0, 0), ImGuiChildFlags_None,
		ImGuiWindowFlags_HorizontalScrollbar);

	// -----------------------------------------------------------------------
	// Root: CONSTRUCTS
	// -----------------------------------------------------------------------
	{
		ImGui::PushStyleColor(ImGuiCol_Header, TnxStyle::Color::BgElev);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, TnxStyle::Color::PurpleFaint);
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, TnxStyle::Color::PurpleSoft);
		ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);
		bool constructsOpen = ImGui::CollapsingHeader("CONSTRUCTS##root",
			ImGuiTreeNodeFlags_DefaultOpen);
		ImGui::PopStyleColor(4);

		if (constructsOpen)
		{
			if (cr && constructCount > 0)
			{
				cr->ForEachWithMeta([&](void* ptr, uint32_t id, const char* rawTypeName)
				{
					const char* displayName = StripTypePrefix(rawTypeName);

					bool selected = (state.Selection == EditorState::SelectionType::Construct
									 && state.SelectedConstructID == id);

					ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf
											   | ImGuiTreeNodeFlags_NoTreePushOnOpen
											   | ImGuiTreeNodeFlags_SpanFullWidth;
					if (selected) flags |= ImGuiTreeNodeFlags_Selected;

					char label[128];
					snprintf(label, sizeof(label), "%s###cnst_%u", displayName, id);

					ImGui::PushStyleColor(ImGuiCol_Text,
						selected ? TnxStyle::Color::PurpleHot : TnxStyle::Color::Fg);
					ImGui::TreeNodeEx(label, flags);
					ImGui::PopStyleColor();

					bool clicked = ImGui::IsItemClicked();

					// Right-align the construct ID, dimmed
					char idStr[24];
					snprintf(idStr, sizeof(idStr), "#%u", id);
					float idW       = ImGui::CalcTextSize(idStr).x;
					float posX      = ImGui::GetWindowWidth() - idW - ImGui::GetScrollX() - 12.0f;
					float labelEndX = ImGui::GetCursorPosX() + ImGui::CalcTextSize(displayName).x + 24.0f;
					if (posX > labelEndX)
					{
						ImGui::SameLine(posX);
						ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
						ImGui::TextUnformatted(idStr);
						ImGui::PopStyleColor();
					}

					if (clicked)
					{
						state.ClearSelection();
						state.Selection                 = EditorState::SelectionType::Construct;
						state.SelectedConstructID       = id;
						state.SelectedConstructPtr      = ptr;
						state.SelectedConstructTypeName = rawTypeName;
					}
				});
			}
			else
			{
				ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
				ImGui::TextUnformatted("  No constructs (simulation not running)");
				ImGui::PopStyleColor();
			}
		}
	}

	// -----------------------------------------------------------------------
	// Root: ARCHETYPES
	// -----------------------------------------------------------------------
	if (state.RegistryPtr)
	{
		ImGui::PushStyleColor(ImGuiCol_Header, TnxStyle::Color::BgElev);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, TnxStyle::Color::PurpleFaint);
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, TnxStyle::Color::PurpleSoft);
		ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);
		bool archetypesOpen = ImGui::CollapsingHeader("ARCHETYPES##root",
			ImGuiTreeNodeFlags_DefaultOpen);
		ImGui::PopStyleColor(4);

		if (archetypesOpen)
		{
			for (auto& [key, arch] : state.RegistryPtr->GetArchetypes())
			{
				if (!arch) continue;

				bool archetypeSelected = (state.Selection == EditorState::SelectionType::Archetype
										  && state.SelectedClassID == key.ID);

				char label[256];
				snprintf(label, sizeof(label), "%s###arch_%u", arch->DebugName, key.ID);

				ImGuiTreeNodeFlags archFlags = ImGuiTreeNodeFlags_OpenOnArrow;
				if (archetypeSelected) archFlags |= ImGuiTreeNodeFlags_Selected;

				bool archOpen = ImGui::TreeNodeEx(label, archFlags);

				bool archClicked = ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen();

				// Right-align entity count
				char countStr[32];
				snprintf(countStr, sizeof(countStr), "%u ent", arch->TotalEntityCount);
				float cntW = ImGui::CalcTextSize(countStr).x;
				float posX = ImGui::GetWindowWidth() - cntW - ImGui::GetScrollX() - 12.0f;
				ImGui::SameLine(posX);
				ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
				ImGui::TextUnformatted(countStr);
				ImGui::PopStyleColor();

				if (archClicked)
				{
					state.ClearSelection();
					state.Selection         = EditorState::SelectionType::Archetype;
					state.SelectedClassID   = key.ID;
					state.SelectedArchetype = arch;
				}

				if (archOpen)
				{
					for (size_t ci = 0; ci < arch->Chunks.size(); ++ci)
					{
						Chunk* chunk       = arch->Chunks[ci];
						uint32_t liveCount = arch->GetLiveChunkCount(ci);

						char chunkLabel[128];
						snprintf(chunkLabel, sizeof(chunkLabel), "chunk %zu  (%u)###chunk_%u_%zu",
							ci, liveCount, key.ID, ci);

						ImGuiTreeNodeFlags chunkFlags = ImGuiTreeNodeFlags_OpenOnArrow;
						bool chunkOpen                = ImGui::TreeNodeEx(chunkLabel, chunkFlags);

						if (chunkOpen)
						{
							for (uint32_t ei = 0; ei < liveCount; ++ei)
							{
								uint32_t cacheIdx = static_cast<uint32_t>(chunk->Header.CacheIndexStart) + ei;

								bool entitySelected = (state.Selection == EditorState::SelectionType::Entity
													   && state.SelectedCacheIndex == cacheIdx);

								char entLabel[64];
								snprintf(entLabel, sizeof(entLabel), "e 0x%06X###ent_%u", cacheIdx, cacheIdx);

								ImGuiTreeNodeFlags entFlags = ImGuiTreeNodeFlags_Leaf
															  | ImGuiTreeNodeFlags_NoTreePushOnOpen;
								if (entitySelected) entFlags |= ImGuiTreeNodeFlags_Selected;

								ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
								ImGui::TreeNodeEx(entLabel, entFlags);
								ImGui::PopStyleColor();

								if (ImGui::IsItemClicked())
								{
									state.ClearSelection();
									state.Selection          = EditorState::SelectionType::Entity;
									state.SelectedClassID    = key.ID;
									state.SelectedArchetype  = arch;
									state.SelectedChunk      = chunk;
									state.SelectedLocalIndex = static_cast<uint16_t>(ei);
									state.SelectedCacheIndex = cacheIdx;
								}

								if (ImGui::BeginPopupContextItem())
								{
									if (ImGui::MenuItem("Delete"))
									{
										state.ClearSelection();
										state.Selection          = EditorState::SelectionType::Entity;
										state.SelectedClassID    = key.ID;
										state.SelectedArchetype  = arch;
										state.SelectedChunk      = chunk;
										state.SelectedLocalIndex = static_cast<uint16_t>(ei);
										state.SelectedCacheIndex = cacheIdx;
										if (state.EditorCtx) state.EditorCtx->DeleteSelectedEntity();
									}
									ImGui::EndPopup();
								}
							}
							ImGui::TreePop();
						}
					}
					ImGui::TreePop();
				}
			}
		}
	}

	// --- Delete key when focused ---
	if (state.Selection == EditorState::SelectionType::Entity
		&& ImGui::IsWindowFocused()
		&& ImGui::IsKeyPressed(ImGuiKey_Delete)
		&& !ImGui::IsAnyItemActive()
		&& state.EditorCtx)
	{
		state.EditorCtx->DeleteSelectedEntity();
	}

	// --- Drop target: accept prefab drags on empty space ---
	ImGuiWindow* win = ImGui::GetCurrentWindow();
	ImRect winRect(win->InnerRect);
	if (ImGui::BeginDragDropTargetCustom(winRect, win->ID))
	{
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PREFAB_PATH"))
		{
			if (state.EditorCtx)
			{
				std::string prefabPath(static_cast<const char*>(payload->Data));
				state.EditorCtx->SpawnPrefab(prefabPath);
			}
		}
		ImGui::EndDragDropTarget();
	}

	ImGui::EndChild();
	ImGui::End();
}
