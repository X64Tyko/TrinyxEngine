#include "Panels/DetailsPanel.h"
#include "EditorState.h"
#include "Archetype.h"
#include "FieldMeta.h"
#include "LogicThread.h"
#include "Registry.h"
#include "imgui.h"

bool DetailsPanel::EditFieldValue(const char* label, size_t fieldSize, void* fieldArray,
								  uint32_t entityIndex, uint8_t valueType)
{
	auto type      = static_cast<FieldValueType>(valueType);
	uint8_t* base  = static_cast<uint8_t*>(fieldArray);
	void* valuePtr = base + entityIndex * fieldSize;

	// Unique ID per field so ImGui doesn't collide
	ImGui::PushID(label);

	bool edited = false;

	switch (type)
	{
		case FieldValueType::Float32:
			{
				float val = *static_cast<float*>(valuePtr);
				if (ImGui::InputFloat("##v", &val, 0.1f, 1.0f, "%.4f"))
				{
					*static_cast<float*>(valuePtr) = val;
					edited                         = true;
				}
				break;
			}
		case FieldValueType::Float64:
			{
				double val = *static_cast<double*>(valuePtr);
				float fval = static_cast<float>(val);
				if (ImGui::InputFloat("##v", &fval, 0.1f, 1.0f, "%.6f"))
				{
					*static_cast<double*>(valuePtr) = static_cast<double>(fval);
					edited                          = true;
				}
				break;
			}
		case FieldValueType::Int32:
			{
				int32_t val = *static_cast<int32_t*>(valuePtr);
				if (ImGui::InputInt("##v", &val))
				{
					*static_cast<int32_t*>(valuePtr) = val;
					edited                           = true;
				}
				break;
			}
		case FieldValueType::Uint32:
			{
				// Display as hex for flags/enums, decimal input
				uint32_t val = *static_cast<uint32_t*>(valuePtr);
				int ival     = static_cast<int>(val);
				if (ImGui::InputInt("##v", &ival))
				{
					*static_cast<uint32_t*>(valuePtr) = static_cast<uint32_t>(ival);
					edited                            = true;
				}
				break;
			}
		default:
			{
				// Unknown or unhandled type — read-only display
				if (fieldSize == sizeof(float))
				{
					ImGui::Text("%.4f", *static_cast<const float*>(valuePtr));
				}
				else
				{
					ImGui::Text("%zu bytes", fieldSize);
				}
				break;
			}
	}

	ImGui::PopID();
	return edited;
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

	// Check if simulation is paused (editing only allowed when paused)
	bool simPaused = state.LogicPtr && state.LogicPtr->IsSimPaused();

	// --- Archetype Info ---
	ImGui::Text("%s", arch->DebugName);
	ImGui::Text("ClassID: %u  |  Entities: %u  |  Chunks: %zu",
				arch->ArchClassID, arch->TotalEntityCount, arch->Chunks.size());
	ImGui::Separator();

	// --- Component List ---
	if (ImGui::CollapsingHeader("Components", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const auto& cfr = ComponentFieldRegistry::Get();

		// Iterate field layout, group by component (fields are contiguous per component)
		ComponentTypeID currentCompID = 0;
		bool treeOpen                 = false;

		for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
		{
			// New component — close previous tree node, open a new one
			if (fdesc.componentID != currentCompID)
			{
				if (treeOpen) ImGui::TreePop();

				currentCompID        = fdesc.componentID;
				const auto* fields   = cfr.GetFields(currentCompID);
				size_t fieldCount    = fields ? fields->size() : 0;
				const char* compName = cfr.GetAllComponents().count(currentCompID)
										   ? cfr.GetComponentMeta(currentCompID).Name
										   : "Unknown";

				char compLabel[128];
				snprintf(compLabel, sizeof(compLabel), "%s  (%zu fields)###comp_%u",
						 compName ? compName : "Unknown", fieldCount, currentCompID);

				treeOpen = ImGui::TreeNodeEx(compLabel, ImGuiTreeNodeFlags_DefaultOpen);
				if (treeOpen)
				{
					ImGui::Columns(2, nullptr, true);
					ImGui::SetColumnWidth(0, 160.0f);
				}
			}

			if (treeOpen)
			{
				// Look up field name from the registry
				const auto* fields = cfr.GetFields(fdesc.componentID);
				const char* name   = (fields && fdesc.componentSlotIndex < fields->size())
									   ? (*fields)[fdesc.componentSlotIndex].Name
									   : "???";

				ImGui::Text("%s", name);
				ImGui::NextColumn();
				ImGui::Text("%zu bytes", fdesc.fieldSize);
				ImGui::NextColumn();
			}
		}

		if (treeOpen)
		{
			ImGui::Columns(1);
			ImGui::TreePop();
		}
	}

	// --- Entity Field Values (only when an entity is selected) ---
	if (state.Selection == EditorState::SelectionType::Entity && state.SelectedChunk)
	{
		ImGui::Separator();
		if (ImGui::CollapsingHeader("Field Values", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (!simPaused)
			{
				ImGui::TextDisabled("Pause simulation to edit values");
			}

			size_t fieldCount = arch->GetFieldArrayCount();
			if (fieldCount > 0 && fieldCount <= MAX_FIELDS_PER_ARCHETYPE)
			{
				void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];

				uint32_t temporalFrame = state.RegistryPtr->GetTemporalCache()->GetActiveWriteFrame();
				uint32_t volatileFrame = state.RegistryPtr->GetVolatileCache()->GetActiveWriteFrame();

				arch->BuildFieldArrayTable(state.SelectedChunk, fieldArrayTable, temporalFrame, volatileFrame);

				const auto& cfr = ComponentFieldRegistry::Get();

				ImGui::Columns(2, "FieldValues", true);
				ImGui::SetColumnWidth(0, 160.0f);

				for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
				{
					size_t idx = fdesc.fieldSlotIndex;

					// Look up field name from the registry
					const auto* fields    = cfr.GetFields(fdesc.componentID);
					const char* fieldName = (fields && fdesc.componentSlotIndex < fields->size())
												? (*fields)[fdesc.componentSlotIndex].Name
												: "???";

					ImGui::Text("%s", fieldName);
					ImGui::NextColumn();

					if (fieldArrayTable[idx])
					{
						if (simPaused)
						{
							bool edited = EditFieldValue(
								fieldName, fdesc.fieldSize, fieldArrayTable[idx],
								state.SelectedLocalIndex,
								static_cast<uint8_t>(fdesc.valueType));

							if (edited)
							{
								state.bSceneDirty = true;
							}
						}
						else
						{
							// Read-only display when sim is running
							uint8_t* base      = static_cast<uint8_t*>(fieldArrayTable[idx]);
							const void* valPtr = base + state.SelectedLocalIndex * fdesc.fieldSize;

							switch (fdesc.valueType)
							{
								case FieldValueType::Float32: ImGui::Text("%.4f", *static_cast<const float*>(valPtr));
									break;
								case FieldValueType::Float64: ImGui::Text("%.6f", *static_cast<const double*>(valPtr));
									break;
								case FieldValueType::Int32: ImGui::Text("%d", *static_cast<const int32_t*>(valPtr));
									break;
								case FieldValueType::Uint32: ImGui::Text("%u", *static_cast<const uint32_t*>(valPtr));
									break;
								default: ImGui::Text("%zu bytes", fdesc.fieldSize);
									break;
							}
						}
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