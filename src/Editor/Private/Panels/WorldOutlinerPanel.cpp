#include "Panels/WorldOutlinerPanel.h"
#include "EditorState.h"
#include "Registry.h"
#include "imgui.h"

void WorldOutlinerPanel::Draw(EditorState& state)
{
	ImGui::Begin(Title, &bVisible);

	if (!state.RegistryPtr)
	{
		ImGui::TextDisabled("No registry");
		ImGui::End();
		return;
	}

	ImGui::Text("Entities: %u  |  Chunks: %u",
				state.RegistryPtr->GetTotalEntityCount(),
				state.RegistryPtr->GetTotalChunkCount());
	ImGui::Separator();

	for (auto& [key, arch] : state.RegistryPtr->GetArchetypes())
	{
		if (!arch) continue;

		// Archetype tree node: "DebugName (N entities)"
		char label[256];
		snprintf(label, sizeof(label), "%s  (%u entities)###arch_%u",
				 arch->DebugName, arch->TotalEntityCount, key.ID);

		bool archetypeSelected = (state.Selection == EditorState::SelectionType::Archetype
			&& state.SelectedClassID == key.ID);

		ImGuiTreeNodeFlags archFlags = ImGuiTreeNodeFlags_OpenOnArrow;
		if (archetypeSelected) archFlags |= ImGuiTreeNodeFlags_Selected;

		bool archOpen = ImGui::TreeNodeEx(label, archFlags);

		// Click on archetype label selects it
		if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
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
				Chunk* chunk         = arch->Chunks[ci];
				uint32_t entityCount = arch->GetChunkCount(ci);

				char chunkLabel[128];
				snprintf(chunkLabel, sizeof(chunkLabel), "Chunk %zu  (%u entities)###chunk_%u_%zu",
						 ci, entityCount, key.ID, ci);

				ImGuiTreeNodeFlags chunkFlags = ImGuiTreeNodeFlags_OpenOnArrow;

				bool chunkOpen = ImGui::TreeNodeEx(chunkLabel, chunkFlags);

				if (chunkOpen)
				{
					// Entity leaves
					for (uint32_t ei = 0; ei < entityCount; ++ei)
					{
						uint32_t cacheIdx = static_cast<uint32_t>(chunk->Header.CacheIndexStart) + ei;

						char entityLabel[64];
						snprintf(entityLabel, sizeof(entityLabel), "Entity %u###ent_%u", cacheIdx, cacheIdx);

						bool entitySelected = (state.Selection == EditorState::SelectionType::Entity
							&& state.SelectedCacheIndex == cacheIdx);

						ImGuiTreeNodeFlags entityFlags = ImGuiTreeNodeFlags_Leaf
							| ImGuiTreeNodeFlags_NoTreePushOnOpen;
						if (entitySelected) entityFlags |= ImGuiTreeNodeFlags_Selected;

						ImGui::TreeNodeEx(entityLabel, entityFlags);

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
					}
					ImGui::TreePop();
				}
			}
			ImGui::TreePop();
		}
	}

	ImGui::End();
}