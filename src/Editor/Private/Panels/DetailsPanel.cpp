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
#include "ReflectionRegistry.h"
#include "Registry.h"
#include "TnxStyle.h"
#include "TnxWidgets.h"
#include "imgui.h"

#include <cstring>

// Map CacheTier enum to the TierBadge display tier.
static TnxWidgets::Tier TierFromCache(CacheTier ct)
{
	switch (ct)
	{
		case CacheTier::Temporal: return TnxWidgets::Tier::Temporal;
		case CacheTier::Volatile: return TnxWidgets::Tier::Volatile;
		case CacheTier::Universal: return TnxWidgets::Tier::Temporal;
		default: return TnxWidgets::Tier::Cold;
	}
}

// Strip MSVC "class "/"struct " prefixes from typeid names.
static const char* StripTypePrefix(const char* name)
{
	if (!name) return "(unknown)";
	if (std::strncmp(name, "class ", 6) == 0) return name + 6;
	if (std::strncmp(name, "struct ", 7) == 0) return name + 7;
	return name;
}

// -----------------------------------------------------------------------
// Component header — CollapsingHeader with TierBadge decoration.
// Returns true when the section is open (fields should be drawn).
// -----------------------------------------------------------------------
static bool DrawComponentHeader(const char* compName, uint32_t compID, CacheTier tier)
{
	ImGui::PushStyleColor(ImGuiCol_Header, TnxStyle::Color::BgElev);
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, TnxStyle::Color::PurpleFaint);
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, TnxStyle::Color::PurpleSoft);

	char label[128];
	snprintf(label, sizeof(label), "%s###comp_%u", compName ? compName : "Unknown", compID);
	bool open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);

	ImGui::PopStyleColor(3);

	// Decorations on the same line
	ImGui::SameLine();
	TnxWidgets::TierBadge(TierFromCache(tier));

	return open;
}

// -----------------------------------------------------------------------
// Empty state — Nothing selected
// -----------------------------------------------------------------------
static void DrawEmptyState()
{
	ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
	ImGui::Spacing();
	ImGui::Spacing();
	float w           = ImGui::GetContentRegionAvail().x;
	const char* line1 = "Nothing selected.";
	const char* line2 = "Click an entity or archetype in the World Outliner.";
	ImGui::SetCursorPosX((w - ImGui::CalcTextSize(line1).x) * 0.5f);
	ImGui::TextUnformatted(line1);
	ImGui::SetCursorPosX((w - ImGui::CalcTextSize(line2).x) * 0.5f);
	ImGui::TextUnformatted(line2);
	ImGui::PopStyleColor();
}

// -----------------------------------------------------------------------
// Construct mode — basic info card (live editing not yet supported)
// -----------------------------------------------------------------------
static void DrawConstructMode(EditorState& state)
{
	const char* typeName = StripTypePrefix(state.SelectedConstructTypeName);

	ImGui::PushFont(TnxStyle::Font::DisplaySemibold);
	ImGui::TextUnformatted(typeName);
	ImGui::PopFont();

	ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);
	char idBuf[32];
	snprintf(idBuf, sizeof(idBuf), "Construct  #%u", state.SelectedConstructID);
	ImGui::TextUnformatted(idBuf);
	ImGui::PopStyleColor();

	ImGui::Spacing();
	ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
	ImGui::TextWrapped("Construct live-editing is not yet implemented. "
					   "Select an entity in the ARCHETYPES section to inspect and edit field values.");
	ImGui::PopStyleColor();
}

// -----------------------------------------------------------------------
// Component list — shared between Archetype and Entity modes.
// Iterates arch->ArchetypeFieldLayout, one CollapsingHeader per component.
// -----------------------------------------------------------------------
static void DrawComponentList(const ReflectionRegistry& cfr, Archetype* arch,
	bool showFields, void** fieldArrayTable,
	uint16_t entityLocalIndex, bool simPaused,
	EditorState& state)
{
	ComponentTypeID currentCompID = 0;
	bool sectionOpen              = false;

	auto closePrevSection = [&]()
	{
		if (sectionOpen)
		{
			if (showFields) ImGui::Unindent(8.0f);
			sectionOpen = false;
		}
	};

	for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
	{
		if (fdesc.componentID != currentCompID)
		{
			closePrevSection();

			currentCompID        = fdesc.componentID;
			const char* compName = cfr.GetAllComponents().count(currentCompID)
									   ? cfr.GetComponentMeta(currentCompID).Name
									   : "Unknown";
			CacheTier tier       = cfr.GetAllComponents().count(currentCompID)
									   ? cfr.GetComponentMeta(currentCompID).TemporalTier
									   : CacheTier::None;

			sectionOpen = DrawComponentHeader(compName, currentCompID, tier);
			if (sectionOpen && showFields) ImGui::Indent(8.0f);
		}

		if (!sectionOpen || !showFields) continue;

		// --- Field row ---
		const auto* fields    = cfr.GetFields(fdesc.componentID);
		const char* fieldName = (fields && fdesc.componentSlotIndex < fields->size())
									? (*fields)[fdesc.componentSlotIndex].Name
									: "???";

		size_t idx = fdesc.fieldSlotIndex;

		if (!fieldArrayTable || !fieldArrayTable[idx])
		{
			ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
			ImGui::TextUnformatted(fieldName);
			ImGui::SameLine();
			ImGui::TextUnformatted("—");
			ImGui::PopStyleColor();
			continue;
		}

		// Asset-ref fields: combo or read-only name
		if (fdesc.refAssetType != AssetType::Invalid)
		{
			uint8_t* base         = static_cast<uint8_t*>(fieldArrayTable[idx]);
			uint32_t* slotPtr     = reinterpret_cast<uint32_t*>(base + entityLocalIndex * fdesc.fieldSize);
			uint32_t slotIdx      = *slotPtr;
			const char* assetName = MeshManager::Get().GetSlotName(slotIdx);

			ImGui::PushID(fieldName);
			ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);
			ImGui::SetNextItemWidth(90.0f);
			ImGui::TextUnformatted(fieldName);
			ImGui::PopStyleColor();
			ImGui::SameLine();

			if (simPaused)
			{
				const char* preview = (!assetName || !*assetName) ? "(none)" : assetName;
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::BeginCombo("##asset", preview))
				{
					uint32_t meshCount = MeshManager::Get().GetMeshCount();
					for (uint32_t i = 0; i < meshCount; ++i)
					{
						const char* name = MeshManager::Get().GetSlotName(i);
						const char* lbl  = (!name || !*name) ? "(unnamed)" : name;
						bool sel         = (i == slotIdx);
						if (ImGui::Selectable(lbl, sel))
						{
							uint32_t oldSlot  = slotIdx;
							*slotPtr          = i;
							state.bSceneDirty = true;
							MarkEntityDirty(state.RegistryPtr, arch, state.SelectedChunk, entityLocalIndex);
							if (state.EditorCtx)
							{
								auto cmd = std::make_unique<ComponentFieldChangeCommand>(
									arch, state.SelectedChunk, entityLocalIndex,
									fieldName, state.RegistryPtr,
									&oldSlot, FieldValueType::Uint32, fdesc.fieldSize);
								cmd->SetNewValue(slotPtr);
								state.EditorCtx->PushCommand(std::move(cmd));
							}
						}
						if (sel) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}

				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MESH_SLOT"))
					{
						uint32_t oldSlot     = slotIdx;
						uint32_t droppedSlot = *static_cast<const uint32_t*>(payload->Data);
						*slotPtr             = droppedSlot;
						state.bSceneDirty    = true;
						MarkEntityDirty(state.RegistryPtr, arch, state.SelectedChunk, entityLocalIndex);
						if (state.EditorCtx)
						{
							auto cmd = std::make_unique<ComponentFieldChangeCommand>(
								arch, state.SelectedChunk, entityLocalIndex,
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
				ImGui::PushFont(TnxStyle::Font::MonoRegular);
				if (assetName && *assetName)
					ImGui::TextUnformatted(assetName);
				else
				{
					char slotStr[32];
					snprintf(slotStr, sizeof(slotStr), "slot %u", slotIdx);
					ImGui::TextUnformatted(slotStr);
				}
				ImGui::PopFont();
			}
			ImGui::PopID();
			continue;
		}

		// Numeric / bool fields
		uint8_t* base  = static_cast<uint8_t*>(fieldArrayTable[idx]);
		void* valuePtr = base + entityLocalIndex * fdesc.fieldSize;
		ImGui::PushID(fieldName);

		if (simPaused)
		{
			switch (fdesc.valueType)
			{
				case FieldValueType::Fixed32:
				{
					SimFloat* pVal  = static_cast<SimFloat*>(valuePtr);
					SimFloat oldVal = *pVal;
					float fv        = pVal->ToFloat();
					if (TnxWidgets::FieldFloat(fieldName, &fv))
					{
						*pVal             = SimFloat(fv);
						state.bSceneDirty = true;
						MarkEntityDirty(state.RegistryPtr, arch, state.SelectedChunk, entityLocalIndex);
						if (state.EditorCtx)
						{
							auto cmd = std::make_unique<ComponentFieldChangeCommand>(
								arch, state.SelectedChunk, entityLocalIndex,
								fieldName, state.RegistryPtr, &oldVal, fdesc.valueType, fdesc.fieldSize);
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
					if (TnxWidgets::FieldFloat(fieldName, pVal))
					{
						state.bSceneDirty = true;
						MarkEntityDirty(state.RegistryPtr, arch, state.SelectedChunk, entityLocalIndex);
						if (state.EditorCtx)
						{
							auto cmd = std::make_unique<ComponentFieldChangeCommand>(
								arch, state.SelectedChunk, entityLocalIndex,
								fieldName, state.RegistryPtr, &oldVal, fdesc.valueType, fdesc.fieldSize);
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
					float fv      = static_cast<float>(*pVal);
					if (TnxWidgets::FieldFloat(fieldName, &fv))
					{
						*pVal             = static_cast<double>(fv);
						state.bSceneDirty = true;
						MarkEntityDirty(state.RegistryPtr, arch, state.SelectedChunk, entityLocalIndex);
						if (state.EditorCtx)
						{
							auto cmd = std::make_unique<ComponentFieldChangeCommand>(
								arch, state.SelectedChunk, entityLocalIndex,
								fieldName, state.RegistryPtr, &oldVal, fdesc.valueType, fdesc.fieldSize);
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
					int iv         = static_cast<int>(*pVal);
					if (TnxWidgets::FieldInt(fieldName, &iv))
					{
						*pVal             = static_cast<int32_t>(iv);
						state.bSceneDirty = true;
						MarkEntityDirty(state.RegistryPtr, arch, state.SelectedChunk, entityLocalIndex);
						if (state.EditorCtx)
						{
							auto cmd = std::make_unique<ComponentFieldChangeCommand>(
								arch, state.SelectedChunk, entityLocalIndex,
								fieldName, state.RegistryPtr, &oldVal, fdesc.valueType, fdesc.fieldSize);
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
					int iv          = static_cast<int>(*pVal);
					if (TnxWidgets::FieldInt(fieldName, &iv))
					{
						*pVal             = static_cast<uint32_t>(iv);
						state.bSceneDirty = true;
						MarkEntityDirty(state.RegistryPtr, arch, state.SelectedChunk, entityLocalIndex);
						if (state.EditorCtx)
						{
							auto cmd = std::make_unique<ComponentFieldChangeCommand>(
								arch, state.SelectedChunk, entityLocalIndex,
								fieldName, state.RegistryPtr, &oldVal, fdesc.valueType, fdesc.fieldSize);
							cmd->SetNewValue(pVal);
							state.EditorCtx->PushCommand(std::move(cmd));
						}
					}
					break;
				}
				default:
					ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
					char info[32];
					snprintf(info, sizeof(info), "%zu bytes", fdesc.fieldSize);
					ImGui::TextUnformatted(info);
					ImGui::PopStyleColor();
					break;
			}
		}
		else
		{
			// Read-only display when sim is running
			ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);
			ImGui::SetNextItemWidth(90.0f);
			ImGui::TextUnformatted(fieldName);
			ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::PushFont(TnxStyle::Font::MonoRegular);
			switch (fdesc.valueType)
			{
				case FieldValueType::Fixed32:
				{
					char vbuf[32];
					snprintf(vbuf, sizeof(vbuf), "%.4f", ((SimFloat*)valuePtr)->ToFloat());
					ImGui::TextUnformatted(vbuf);
					break;
				}
				case FieldValueType::Float32:
				{
					char vbuf[32];
					snprintf(vbuf, sizeof(vbuf), "%.4f", *static_cast<const float*>(valuePtr));
					ImGui::TextUnformatted(vbuf);
					break;
				}
				case FieldValueType::Float64:
				{
					char vbuf[32];
					snprintf(vbuf, sizeof(vbuf), "%.6f", *static_cast<const double*>(valuePtr));
					ImGui::TextUnformatted(vbuf);
					break;
				}
				case FieldValueType::Int32:
				{
					char vbuf[32];
					snprintf(vbuf, sizeof(vbuf), "%d", *static_cast<const int32_t*>(valuePtr));
					ImGui::TextUnformatted(vbuf);
					break;
				}
				case FieldValueType::Uint32:
				{
					char vbuf[32];
					snprintf(vbuf, sizeof(vbuf), "%u", *static_cast<const uint32_t*>(valuePtr));
					ImGui::TextUnformatted(vbuf);
					break;
				}
				default:
				{
					char vbuf[32];
					snprintf(vbuf, sizeof(vbuf), "%zu bytes", fdesc.fieldSize);
					ImGui::TextUnformatted(vbuf);
					break;
				}
			}
			ImGui::PopFont();
		}

		ImGui::PopID();
	}

	closePrevSection();
}

// -----------------------------------------------------------------------
// Draw
// -----------------------------------------------------------------------

void DetailsPanel::Draw(EditorState& state)
{
	if (!BeginPadded())
	{
		ImGui::End();
		return;
	}
	TnxWidgets::PanelHeader(nullptr, "Details");

	// --- None ---
	if (state.Selection == EditorState::SelectionType::None)
	{
		DrawEmptyState();
		ImGui::End();
		return;
	}

	// --- Construct ---
	if (state.Selection == EditorState::SelectionType::Construct)
	{
		DrawConstructMode(state);
		ImGui::End();
		return;
	}

	// --- Archetype / Entity: need a valid arch ---
	Archetype* arch = state.SelectedArchetype;
	if (!arch)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
		ImGui::TextUnformatted("Invalid selection");
		ImGui::PopStyleColor();
		ImGui::End();
		return;
	}

	bool simPaused = state.LogicPtr && state.LogicPtr->IsSimPaused();

	// --- Archetype header ---
	ImGui::PushFont(TnxStyle::Font::DisplaySemibold);
	ImGui::TextUnformatted(arch->DebugName);
	ImGui::PopFont();

	ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);
	char hdrBuf[128];
	snprintf(hdrBuf, sizeof(hdrBuf), "ClassID %u  ·  %u entities  ·  %zu chunks",
		arch->ArchClassID, arch->TotalEntityCount, arch->Chunks.size());
	ImGui::TextUnformatted(hdrBuf);
	ImGui::PopStyleColor();

	if (state.Selection == EditorState::SelectionType::Entity)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
		char entBuf[64];
		snprintf(entBuf, sizeof(entBuf), "Entity  idx 0x%06X  ·  local %u",
			state.SelectedCacheIndex, state.SelectedLocalIndex);
		ImGui::TextUnformatted(entBuf);
		ImGui::PopStyleColor();
	}

	if (!simPaused && state.Selection == EditorState::SelectionType::Entity)
	{
		ImGui::Spacing();
		TnxWidgets::Chip("Pause simulation to edit", TnxWidgets::ChipStyle::Warn, false);
	}

	ImGui::Spacing();

	const auto& cfr = ReflectionRegistry::Get();

	// --- Archetype mode: read-only component list with tier badges ---
	if (state.Selection == EditorState::SelectionType::Archetype)
	{
		DrawComponentList(cfr, arch,
			/* showFields */ false, nullptr,
			0, false, state);
		ImGui::End();
		return;
	}

	// --- Entity mode: editable field values per component ---
	if (!state.SelectedChunk)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
		ImGui::TextUnformatted("No chunk selected");
		ImGui::PopStyleColor();
		ImGui::End();
		return;
	}

	size_t fieldCount = arch->GetFieldArrayCount();
	if (fieldCount == 0 || fieldCount > MAX_FIELDS_PER_ARCHETYPE)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
		ImGui::TextUnformatted("No field data available");
		ImGui::PopStyleColor();
		ImGui::End();
		return;
	}

	void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
	uint32_t temporalFrame = state.RegistryPtr->GetTemporalCache()->GetActiveWriteFrame();
	uint32_t volatileFrame = state.RegistryPtr->GetVolatileCache()->GetActiveWriteFrame();
	arch->BuildFieldArrayTable(state.SelectedChunk, fieldArrayTable, temporalFrame, volatileFrame);

	DrawComponentList(cfr, arch,
		/* showFields */ true, fieldArrayTable,
		state.SelectedLocalIndex, simPaused, state);

	ImGui::End();
}
