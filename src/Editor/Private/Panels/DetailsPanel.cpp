#include "Panels/DetailsPanel.h"
#include "EditorState.h"
#include "UndoCommand.h"
#include "EditorContext.h"
#include "Archetype.h"
#include "AssetTypes.h"
#include "CacheSlotMeta.h"
#include "FieldMeta.h"
#include "LogicThreadBase.h"
#include "MeshManager.h"
#include "Registry.h"
#include "imgui.h"

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
		const auto& cfr = ReflectionRegistry::Get();

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

				const auto& cfr = ReflectionRegistry::Get();

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
						// Asset-ref fields: combo selector when paused, name display when running
						if (fdesc.refAssetType != AssetType::Invalid && state.MeshMgrPtr)
						{
							uint8_t* base                = static_cast<uint8_t*>(fieldArrayTable[idx]);
							uint32_t* slotPtr            = reinterpret_cast<uint32_t*>(base + state.SelectedLocalIndex * fdesc.fieldSize);
							uint32_t slotIdx      = *slotPtr;
							const char* assetName = state.MeshMgrPtr->GetSlotName(slotIdx);

							ImGui::PushID(fieldName);

							if (simPaused)
							{
								// Combo dropdown of available assets of this type
								const char* preview = (!assetName || !*assetName) ? "(none)" : assetName;
								ImGui::SetNextItemWidth(-1);
								if (ImGui::BeginCombo("##asset", preview))
								{
									uint32_t meshCount = state.MeshMgrPtr->GetMeshCount();
									for (uint32_t i = 0; i < meshCount; ++i)
									{
										const char* name  = state.MeshMgrPtr->GetSlotName(i);
										const char* label = (!name || !*name) ? "(unnamed)" : name;
										bool selected     = (i == slotIdx);
										if (ImGui::Selectable(label, selected))
										{
											uint32_t oldSlot  = slotIdx; // before change
											*slotPtr          = i;
											state.bSceneDirty = true;
											MarkEntityDirty(state.RegistryPtr, arch, state.SelectedChunk, state.SelectedLocalIndex);

											// Push undo command
											if (state.EditorCtx)
											{
												auto cmd = std::make_unique<ComponentFieldChangeCommand>(
													arch, state.SelectedChunk, state.SelectedLocalIndex,
													fieldName, state.RegistryPtr,
													&oldSlot, FieldValueType::Uint32, fdesc.fieldSize);
												cmd->SetNewValue(slotPtr);
												state.EditorCtx->PushCommand(std::move(cmd));
											}
										}
										if (selected) ImGui::SetItemDefaultFocus();
									}
									ImGui::EndCombo();
								}

								// Drop target: accept mesh drags from content browser
								if (ImGui::BeginDragDropTarget())
								{
									if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MESH_SLOT"))
									{
										uint32_t oldSlot     = slotIdx;
										uint32_t droppedSlot = *static_cast<const uint32_t*>(payload->Data);
										*slotPtr             = droppedSlot;
										state.bSceneDirty    = true;
										MarkEntityDirty(state.RegistryPtr, arch, state.SelectedChunk, state.SelectedLocalIndex);

										// Push undo command
										if (state.EditorCtx)
										{
											auto cmd = std::make_unique<ComponentFieldChangeCommand>(
												arch, state.SelectedChunk, state.SelectedLocalIndex,
												fieldName, state.RegistryPtr,
												&oldSlot, FieldValueType::Uint32, fdesc.fieldSize);
											cmd->SetNewValue(slotPtr);
											state.EditorCtx->PushCommand(std::move(cmd));
										}
									}
									ImGui::EndDragDropTarget();
								}
							}
							else
							{
								// Read-only name display when sim is running
								if (assetName && *assetName) ImGui::Text("%s", assetName);
								else ImGui::Text("slot %u", slotIdx);
							}

							ImGui::PopID();
						}
						else if (simPaused)
						{
							uint8_t* base  = static_cast<uint8_t*>(fieldArrayTable[idx]);
							void* valuePtr = base + state.SelectedLocalIndex * fdesc.fieldSize;
							ImGui::PushID(fieldName);

							switch (fdesc.valueType)
							{
								case FieldValueType::Fixed32:
									{
										SimFloat* pVal  = static_cast<SimFloat*>(valuePtr);
										SimFloat oldVal = *pVal;
										float floatVal  = pVal->ToFloat();
										if (ImGui::InputFloat("##v", &floatVal, 0.1f, 1.0f, "%.4f"))
										{
											*pVal             = SimFloat(floatVal);
											state.bSceneDirty = true;
											MarkEntityDirty(state.RegistryPtr, arch, state.SelectedChunk, state.SelectedLocalIndex);
											if (state.EditorCtx)
											{
												auto cmd = std::make_unique<ComponentFieldChangeCommand>(
													arch, state.SelectedChunk, state.SelectedLocalIndex,
													fieldName, state.RegistryPtr, &oldVal,
													fdesc.valueType, fdesc.fieldSize);
												cmd->SetNewValue(pVal);
												state.EditorCtx->PushCommand(std::move(cmd));
											}
										}
										break;
									}
								case FieldValueType::Float32:
									{
										float* pVal  = static_cast<float*>(valuePtr);
										float oldVal = *pVal;
										if (ImGui::InputFloat("##v", pVal, 0.1f, 1.0f, "%.4f"))
										{
											state.bSceneDirty = true;
											MarkEntityDirty(state.RegistryPtr, arch, state.SelectedChunk, state.SelectedLocalIndex);
											if (state.EditorCtx)
											{
												auto cmd = std::make_unique<ComponentFieldChangeCommand>(
													arch, state.SelectedChunk, state.SelectedLocalIndex,
													fieldName, state.RegistryPtr, &oldVal,
													fdesc.valueType, fdesc.fieldSize);
												cmd->SetNewValue(pVal);
												state.EditorCtx->PushCommand(std::move(cmd));
											}
										}
										break;
									}
								case FieldValueType::Float64:
									{
										double* pVal  = static_cast<double*>(valuePtr);
										double oldVal = *pVal;
										float fVal    = static_cast<float>(*pVal);
										if (ImGui::InputFloat("##v", &fVal, 0.1f, 1.0f, "%.6f"))
										{
											*pVal             = static_cast<double>(fVal);
											state.bSceneDirty = true;
											MarkEntityDirty(state.RegistryPtr, arch, state.SelectedChunk, state.SelectedLocalIndex);
											if (state.EditorCtx)
											{
												auto cmd = std::make_unique<ComponentFieldChangeCommand>(
													arch, state.SelectedChunk, state.SelectedLocalIndex,
													fieldName, state.RegistryPtr, &oldVal,
													fdesc.valueType, fdesc.fieldSize);
												cmd->SetNewValue(pVal);
												state.EditorCtx->PushCommand(std::move(cmd));
											}
										}
										break;
									}
								case FieldValueType::Int32:
									{
										int32_t* pVal  = static_cast<int32_t*>(valuePtr);
										int32_t oldVal = *pVal;
										if (ImGui::InputInt("##v", pVal))
										{
											state.bSceneDirty = true;
											MarkEntityDirty(state.RegistryPtr, arch, state.SelectedChunk, state.SelectedLocalIndex);
											if (state.EditorCtx)
											{
												auto cmd = std::make_unique<ComponentFieldChangeCommand>(
													arch, state.SelectedChunk, state.SelectedLocalIndex,
													fieldName, state.RegistryPtr, &oldVal,
													fdesc.valueType, fdesc.fieldSize);
												cmd->SetNewValue(pVal);
												state.EditorCtx->PushCommand(std::move(cmd));
											}
										}
										break;
									}
								case FieldValueType::Uint32:
									{
										uint32_t* pVal  = static_cast<uint32_t*>(valuePtr);
										uint32_t oldVal = *pVal;
										int iVal        = static_cast<int>(*pVal);
										if (ImGui::InputInt("##v", &iVal))
										{
											*pVal             = static_cast<uint32_t>(iVal);
											state.bSceneDirty = true;
											MarkEntityDirty(state.RegistryPtr, arch, state.SelectedChunk, state.SelectedLocalIndex);
											if (state.EditorCtx)
											{
												auto cmd = std::make_unique<ComponentFieldChangeCommand>(
													arch, state.SelectedChunk, state.SelectedLocalIndex,
													fieldName, state.RegistryPtr, &oldVal,
													fdesc.valueType, fdesc.fieldSize);
												cmd->SetNewValue(pVal);
												state.EditorCtx->PushCommand(std::move(cmd));
											}
										}
										break;
									}
								default: break;
							}
							ImGui::PopID();
						}
						else
						{
							// Read-only display when sim is running
							uint8_t* base      = static_cast<uint8_t*>(fieldArrayTable[idx]);
							const void* valPtr = base + state.SelectedLocalIndex * fdesc.fieldSize;

							switch (fdesc.valueType)
							{
								case FieldValueType::Fixed32: ImGui::Text("%.4f", ((Fixed32*)valPtr)->ToFloat());
									break;
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
