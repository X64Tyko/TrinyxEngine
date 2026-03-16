#include "Panels/DetailsPanel.h"
#include "EditorState.h"
#include "Archetype.h"
#include "FieldMeta.h"
#include "LogicThread.h"
#include "Registry.h"
#include "imgui.h"

static void DisplayFieldValue(size_t fieldSize, const void* fieldArray, uint32_t entityIndex)
{
	const uint8_t* base  = static_cast<const uint8_t*>(fieldArray);
	const void* valuePtr = base + entityIndex * fieldSize;

	// Display based on field size (heuristic: 4 bytes = float/uint32, 8 = double/uint64)
	if (fieldSize == sizeof(float))
	{
		float val = *static_cast<const float*>(valuePtr);
		ImGui::Text("%.4f", val);
	}
	else if (fieldSize == sizeof(double))
	{
		double val = *static_cast<const double*>(valuePtr);
		ImGui::Text("%.6f", val);
	}
	else
	{
		ImGui::Text("%zu bytes", fieldSize);
	}
}

void DetailsPanel::Draw(EditorState& state)
{
	ImGui::Begin(Title, &bVisible);

	if (state.Selection == EditorState::SelectionType::None)
	{
		ImGui::TextDisabled("Select an entity or archetype in the World Outliner");
		ImGui::End();
		return;
	}

	Archetype* arch = state.SelectedArchetype;
	if (!arch)
	{
		ImGui::TextDisabled("Invalid selection");
		ImGui::End();
		return;
	}

	// --- Archetype Info ---
	ImGui::Text("%s", arch->DebugName);
	ImGui::Text("ClassID: %u  |  Entities: %u  |  Chunks: %zu",
				arch->ArchClassID, arch->TotalEntityCount, arch->Chunks.size());
	ImGui::Separator();

	// --- Component List ---
	if (ImGui::CollapsingHeader("Components", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const auto& cfr = ComponentFieldRegistry::Get();

		for (auto& [typeID, meta] : arch->ComponentLayout)
		{
			char compLabel[128];
			const auto* fields = cfr.GetFields(typeID);
			size_t fieldCount  = fields ? fields->size() : 0;

			snprintf(compLabel, sizeof(compLabel), "Component %u  (%zu fields)###comp_%u",
					 typeID, fieldCount, typeID);

			if (ImGui::TreeNodeEx(compLabel, ImGuiTreeNodeFlags_DefaultOpen))
			{
				if (fields && !fields->empty())
				{
					ImGui::Columns(2, nullptr, true);
					ImGui::SetColumnWidth(0, 160.0f);
					for (auto& field : *fields)
					{
						ImGui::Text("%s", field.Name);
						ImGui::NextColumn();
						ImGui::Text("%zu bytes (align %zu)", field.Size, field.Alignment);
						ImGui::NextColumn();
					}
					ImGui::Columns(1);
				}
				else
				{
					ImGui::TextDisabled("No decomposed fields");
				}
				ImGui::TreePop();
			}
		}
	}

	// --- Entity Field Values (only when an entity is selected) ---
	if (state.Selection == EditorState::SelectionType::Entity && state.SelectedChunk)
	{
		ImGui::Separator();
		if (ImGui::CollapsingHeader("Field Values", ImGuiTreeNodeFlags_DefaultOpen))
		{
			size_t fieldCount = arch->CachedFieldArrayLayout.size();
			if (fieldCount > 0 && fieldCount <= MAX_FIELDS_PER_ARCHETYPE)
			{
				void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];

				uint32_t temporalFrame = state.RegistryPtr->GetTemporalCache()->GetActiveWriteFrame();
				uint32_t volatileFrame = state.RegistryPtr->GetVolatileCache()->GetActiveWriteFrame();

				arch->BuildFieldArrayTable(state.SelectedChunk, fieldArrayTable, temporalFrame, volatileFrame);

				ImGui::Columns(2, "FieldValues", true);
				ImGui::SetColumnWidth(0, 160.0f);

				for (size_t i = 0; i < fieldCount; ++i)
				{
					const auto& desc = arch->CachedFieldArrayLayout[i];
					if (!desc.isDecomposed) continue;

					// Use the template cache for the field name — it's stored
					// directly during BuildLayout and doesn't depend on componentID mapping.
					const char* fieldName = arch->FieldArrayTemplateCache[i].debugName;

					ImGui::Text("%s", fieldName);
					ImGui::NextColumn();

					if (fieldArrayTable[i])
					{
						DisplayFieldValue(desc.Size, fieldArrayTable[i], state.SelectedLocalIndex);
					}
					else
					{
						ImGui::TextDisabled("null");
					}
					ImGui::NextColumn();
				}

				ImGui::Columns(1);
			}
		}
	}

	ImGui::End();
}