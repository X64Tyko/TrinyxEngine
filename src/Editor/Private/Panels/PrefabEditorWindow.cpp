#include "Panels/PrefabEditorWindow.h"
#if defined(TNX_ENABLE_EDITOR)

#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include "EditorRenderer.h"
#include "EditorState.h"
#include "EntityBuilder.h"
#include "FlowManager.h"
#include "Globals.h"
#include "Json.h"
#include "MeshManager.h"
#include "ReflectionRegistry.h"
#include "Registry.h"
#include "TnxStyle.h"
#include "TnxWidgets.h"
#include "TrinyxEngine.h"
#include "WorldViewport.h"
#include "imgui.h"

static constexpr uint32_t kPreviewWidth  = 512;
static constexpr uint32_t kPreviewHeight = 512;

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

PrefabEditorWindow::PrefabEditorWindow()
	: EditorPanel("Prefab Editor")
{
	Docs.reserve(16);
}

// ---------------------------------------------------------------------------
// Open / focus
// ---------------------------------------------------------------------------

void PrefabEditorWindow::OpenPrefab(const std::string& filePath, EditorState& state)
{
	for (int i = 0; i < static_cast<int>(Docs.size()); ++i)
	{
		if (Docs[i].FilePath == filePath)
		{
			ActiveTab = i;
			return;
		}
	}

	PrefabDoc doc;
	doc.FilePath = filePath;

	std::ifstream f(filePath);
	if (f.is_open())
	{
		std::ostringstream ss;
		ss << f.rdbuf();
		doc.RootJson = JsonParse(ss.str());
	}

	ParseFromJson(doc);

	// Push the doc into Docs BEFORE calling RebuildPreviewWorld so that
	// PreviewFlow->Config stores a pointer to the vector-resident PreviewConfig,
	// not a now-dead stack address. RewireAllPreviewConfigs updates any existing
	// docs whose addresses shifted due to vector reallocation.
	ActiveTab = static_cast<int>(Docs.size());
	Docs.push_back(std::move(doc));
	RewireAllPreviewConfigs();

	if (Docs.back().PrefabType != "Construct" && !Docs.back().BaseTypeName.empty())
		RebuildPreviewWorld(Docs.back(), state);
}

void PrefabEditorWindow::DestroyAllPreviews(EditorState& state)
{
	for (auto& doc : Docs)
		DestroyPreviewWorld(doc, state);
}

void PrefabEditorWindow::RewireAllPreviewConfigs()
{
	for (auto& doc : Docs)
	{
		if (doc.PreviewFlow)
			doc.PreviewFlow->RewireConfig(&doc.PreviewConfig);
	}
}

// ---------------------------------------------------------------------------
// JSON parsing
// ---------------------------------------------------------------------------

void PrefabEditorWindow::ParseFromJson(PrefabDoc& doc)
{
	const JsonValue* nameVal = doc.RootJson.Find("name");
	doc.PrefabName           = (nameVal && nameVal->IsString()) ? nameVal->AsString() : "Untitled";

	const JsonValue* prefabType = doc.RootJson.Find("prefabType");
	if (prefabType && prefabType->IsString())
		doc.PrefabType = prefabType->AsString();
	else
		doc.PrefabType = "Entity";

	if (doc.PrefabType == "Construct")
	{
		const JsonValue* ct = doc.RootJson.Find("constructType");
		doc.BaseTypeName    = (ct && ct->IsString()) ? ct->AsString() : "";
	}
	else
	{
		const JsonValue* typeVal = doc.RootJson.Find("type");
		doc.BaseTypeName         = (typeVal && typeVal->IsString()) ? typeVal->AsString() : "";
	}
}

// ---------------------------------------------------------------------------
// Preview world lifecycle
// ---------------------------------------------------------------------------

void PrefabEditorWindow::RebuildPreviewWorld(PrefabDoc& doc, EditorState& state)
{
	DestroyPreviewWorld(doc, state);
	doc.PreviewError.clear();

	if (doc.BaseTypeName.empty())
	{
		doc.PreviewError = "No entity type specified.";
		return;
	}
	auto& reg = ReflectionRegistry::Get();
	if (reg.NameToClassID.find(doc.BaseTypeName) == reg.NameToClassID.end())
	{
		doc.PreviewError = "Entity type '" + doc.BaseTypeName + "' not found in registry.";
		return;
	}

	auto* renderer = state.EnginePtr->GetRenderer();
	if (!renderer)
	{
		doc.PreviewError = "Renderer not available.";
		return;
	}

	doc.PreviewConfig = *state.EnginePtr->GetGameConfig();
	doc.PreviewFlow   = std::make_unique<PIELocalFlow>();
	doc.PreviewFlow->Initialize(state.EnginePtr, &doc.PreviewConfig, kPreviewWidth, kPreviewHeight);

	if (!doc.PreviewFlow->CreateWorld())
	{
		doc.PreviewFlow.reset();
		doc.PreviewError = "Preview world creation failed.";
		return;
	}

	WorldBase* world = doc.PreviewFlow->GetWorld();
	world->SetJobsInitialized(true);

	doc.PreviewViewport                   = std::make_unique<WorldViewport>();
	doc.PreviewViewport->TargetWorld      = world;
	doc.PreviewViewport->bUseEditorCamera = true;
	renderer->AllocateViewportResources(doc.PreviewViewport.get(), kPreviewWidth, kPreviewHeight);
	renderer->AddViewport(doc.PreviewViewport.get());

	doc.PreviewFlow->StartWorld();

	// Spawn the entity directly from the in-memory JSON.
	Registry* entityReg = world->GetRegistry();
	JsonValue spawnJson = doc.RootJson; // copy — SpawnEntity may modify in-place
	world->SpawnAndWait([entityReg, &spawnJson](uint32_t)
	{
		EntityBuilder::SpawnEntity(entityReg, spawnJson, false);
	});

	UpdateOrbitCamera(doc);
	doc.bPreviewReady = true;
}

void PrefabEditorWindow::DestroyPreviewWorld(PrefabDoc& doc, EditorState& state)
{
	if (!doc.PreviewFlow) return;

	WorldBase* world = doc.PreviewFlow->GetWorld();
	if (world && world->GetLogicThread() && world->GetLogicThread()->IsSimPaused())
		world->GetLogicThread()->SetSimPaused(false);

	doc.PreviewFlow->StopWorld();
	doc.PreviewFlow->JoinWorld();

	auto* renderer = state.EnginePtr->GetRenderer();
	if (renderer && doc.PreviewViewport)
	{
		renderer->WaitForGPU();
		renderer->RemoveViewport(doc.PreviewViewport.get());
		renderer->FreeViewportResources(doc.PreviewViewport.get());
	}

	doc.PreviewViewport.reset();
	doc.PreviewFlow.reset();
	doc.bPreviewReady = false;
}

// ---------------------------------------------------------------------------
// Orbit camera
// ---------------------------------------------------------------------------

void PrefabEditorWindow::UpdateOrbitCamera(PrefabDoc& doc)
{
	if (!doc.PreviewViewport) return;

	float sinY = std::sin(doc.OrbitYaw), cosY = std::cos(doc.OrbitYaw);
	float sinP = std::sin(doc.OrbitPitch), cosP = std::cos(doc.OrbitPitch);

	// Position orbiting around origin, looking inward.
	float px = -sinY * cosP * doc.OrbitDist;
	float py = sinP * doc.OrbitDist;
	float pz = cosY * cosP * doc.OrbitDist;

	doc.PreviewViewport->Camera.SetPose(px, py, pz, doc.OrbitYaw, -doc.OrbitPitch);
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

void PrefabEditorWindow::SaveDoc(PrefabDoc& doc)
{
	std::ofstream f(doc.FilePath, std::ios::trunc);
	if (!f.is_open()) return;
	std::string json = JsonWrite(doc.RootJson, true);
	f.write(json.data(), static_cast<std::streamsize>(json.size()));
	doc.bDirty = false;
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

void PrefabEditorWindow::Draw(EditorState& state)
{
	// Handle deferred preview rebuilds (triggered by field edits).
	for (auto& doc : Docs)
	{
		if (doc.bPreviewRebuildPending)
		{
			doc.bPreviewRebuildPending = false;
			if (doc.PrefabType != "Construct" && !doc.BaseTypeName.empty())
				RebuildPreviewWorld(doc, state);
		}
		// Keep the orbit camera overriding VizInput drift every frame.
		if (doc.bPreviewReady)
			UpdateOrbitCamera(doc);
	}

	// Remove closed tabs (back-to-front).
	for (int i = static_cast<int>(Docs.size()) - 1; i >= 0; --i)
	{
		if (!Docs[i].bTabOpen)
		{
			DestroyPreviewWorld(Docs[i], state);
			Docs.erase(Docs.begin() + i);
			RewireAllPreviewConfigs();
			if (ActiveTab >= static_cast<int>(Docs.size()))
				ActiveTab = static_cast<int>(Docs.size()) - 1;
		}
	}

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	if (!ImGui::Begin("Prefab Editor"))
	{
		ImGui::PopStyleVar();
		ImGui::End();
		return;
	}
	ImGui::PopStyleVar();

	if (Docs.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
		ImGui::SetCursorPos(ImVec2(20, 40));
		ImGui::TextUnformatted("No prefab open — double-click a .tnxprefab in the Content Browser.");
		ImGui::PopStyleColor();
		ImGui::End();
		return;
	}

	DrawTabBar(state);

	if (ActiveTab < 0 || ActiveTab >= static_cast<int>(Docs.size()))
	{
		ImGui::End();
		return;
	}

	PrefabDoc& doc = Docs[ActiveTab];

	// Two-column split: inspector left, viewport right.
	float totalW = ImGui::GetContentRegionAvail().x;
	float totalH = ImGui::GetContentRegionAvail().y;
	float inspW  = std::max(200.0f, totalW * 0.30f);
	float vpW    = totalW - inspW;

	ImGui::BeginGroup();
	DrawInspector(doc, inspW, state);
	ImGui::EndGroup();

	ImGui::SameLine();

	if (doc.bPreviewReady && doc.PreviewViewport && doc.PreviewViewport->ImGuiTexture != VK_NULL_HANDLE)
	{
		DrawPreviewViewport(doc, vpW, totalH, state);
	}
	else
	{
		ImGui::BeginChild("##nopv", ImVec2(vpW, totalH));
		ImGui::SetCursorPosY(totalH * 0.45f);
		if (!doc.PreviewError.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::Warn);
			float tw = ImGui::CalcTextSize(doc.PreviewError.c_str()).x;
			ImGui::SetCursorPosX((vpW - std::min(tw, vpW - 24.0f)) * 0.5f);
			ImGui::TextWrapped("%s", doc.PreviewError.c_str());
			ImGui::PopStyleColor();
		}
		else
		{
			ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
			float tw = ImGui::CalcTextSize("No preview available").x;
			ImGui::SetCursorPosX((vpW - tw) * 0.5f);
			ImGui::TextUnformatted("No preview available");
			ImGui::PopStyleColor();
		}
		ImGui::EndChild();
	}

	ImGui::End();
}

// ---------------------------------------------------------------------------
// Tab bar
// ---------------------------------------------------------------------------

void PrefabEditorWindow::DrawTabBar(EditorState& state)
{
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 5));
	if (ImGui::BeginTabBar("##prefab_tabs", ImGuiTabBarFlags_AutoSelectNewTabs))
	{
		for (int i = 0; i < static_cast<int>(Docs.size()); ++i)
		{
			PrefabDoc& doc = Docs[i];

			std::string label = doc.PrefabName;
			if (doc.bDirty) label += " *";
			label += "##tab" + std::to_string(i);

			ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
			bool open               = doc.bTabOpen;
			if (ImGui::BeginTabItem(label.c_str(), &open, flags))
			{
				ActiveTab = i;
				ImGui::EndTabItem();
			}
			if (!open) doc.bTabOpen = false;
		}

		// Save / Revert pinned to the right.
		ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - 130.0f);
		if (ActiveTab >= 0 && ActiveTab < static_cast<int>(Docs.size()))
		{
			PrefabDoc& active = Docs[ActiveTab];
			if (TnxWidgets::ButtonPrimary("Save") && active.bDirty)
				SaveDoc(active);

			ImGui::SameLine();

			ImGui::BeginDisabled(!active.bDirty);
			if (ImGui::SmallButton("Revert"))
			{
				std::ifstream f(active.FilePath);
				if (f.is_open())
				{
					std::ostringstream ss;
					ss << f.rdbuf();
					active.RootJson = JsonParse(ss.str());
					ParseFromJson(active);
					active.bDirty                 = false;
					active.bPreviewRebuildPending = true;
				}
			}
			ImGui::EndDisabled();
		}

		ImGui::EndTabBar();
	}
	ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Inspector
// ---------------------------------------------------------------------------

void PrefabEditorWindow::DrawInspector(PrefabDoc& doc, float width, EditorState& state)
{
	ImGui::BeginChild("##inspector", ImVec2(width, 0), false);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));

	// Header.
	ImGui::PushFont(TnxStyle::Font::DisplaySemibold);
	ImGui::TextUnformatted(doc.PrefabName.c_str());
	ImGui::PopFont();

	ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);
	char sub[128];
	snprintf(sub, sizeof(sub), "%s  ›  %s",
		doc.PrefabType.c_str(),
		doc.BaseTypeName.empty() ? "(no type)" : doc.BaseTypeName.c_str());
	ImGui::TextUnformatted(sub);
	ImGui::PopStyleColor();

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	if (doc.PrefabType == "Construct")
	{
		// Validate the construct type is registered.
		auto& reflReg  = ReflectionRegistry::Get();
		bool typeKnown = false;
		for (const auto& entry : reflReg.RegisteredConstructs)
		{
			if (entry.Name && doc.BaseTypeName == entry.Name)
			{
				typeKnown = true;
				break;
			}
		}
		if (!doc.BaseTypeName.empty() && !typeKnown)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::Warn);
			ImGui::TextWrapped("'%s' is not a registered Construct type.", doc.BaseTypeName.c_str());
			ImGui::PopStyleColor();
			ImGui::Spacing();
		}

		// Ensure the "defaults" object exists in the JSON.
		if (!doc.RootJson.Find("defaults"))
			doc.RootJson["defaults"] = JsonValue::Object();

		JsonValue* defs = doc.RootJson.Find("defaults");

		ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);
		ImGui::TextUnformatted("Defaults");
		ImGui::PopStyleColor();
		ImGui::Separator();
		ImGui::Spacing();

		if (defs && defs->IsObject())
		{
			auto& pairs = defs->GetObject();
			int rmAt    = -1;

			for (int pi = 0; pi < static_cast<int>(pairs.size()); ++pi)
			{
				auto& [k, v] = pairs[pi];
				ImGui::PushID(pi);

				// Key label.
				ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);
				ImGui::TextUnformatted(k.c_str());
				ImGui::PopStyleColor();
				ImGui::SameLine();

				// Remove (×) button.
				ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, TnxStyle::Color::BgElev);
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, TnxStyle::Color::BgElev);
				if (ImGui::SmallButton("x")) rmAt = pi;
				ImGui::PopStyleColor(4);
				ImGui::SameLine();

				// Value editor.
				ImGui::SetNextItemWidth(-1.0f);
				if (v.IsString())
				{
					char buf[256] = {};
					std::strncpy(buf, v.AsString().c_str(), sizeof(buf) - 1);
					if (ImGui::InputText("##dv", buf, sizeof(buf)))
					{
						v          = JsonValue::String(buf);
						doc.bDirty = true;
					}
				}
				else if (v.IsNumber())
				{
					float fv = v.AsFloat();
					if (ImGui::InputFloat("##dv", &fv, 0.0f, 0.0f, "%.4g"))
					{
						v          = JsonValue::Number(static_cast<double>(fv));
						doc.bDirty = true;
					}
				}
				else
				{
					ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
					ImGui::TextUnformatted("(unsupported value type)");
					ImGui::PopStyleColor();
				}

				ImGui::PopID();
			}

			if (rmAt >= 0)
			{
				pairs.erase(pairs.begin() + rmAt);
				doc.bDirty = true;
			}
		}

		// Add new default key-value pair.
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		static char s_NewKey[128] = {};
		static char s_NewVal[256] = {};
		ImGui::SetNextItemWidth(100.0f);
		ImGui::InputText("Key##nk", s_NewKey, sizeof(s_NewKey));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-60.0f);
		ImGui::InputText("##nv", s_NewVal, sizeof(s_NewVal));
		ImGui::SameLine();
		if (TnxWidgets::ButtonPrimary("Add") && s_NewKey[0] != '\0')
		{
			(*defs)[s_NewKey] = JsonValue::String(s_NewVal);
			doc.bDirty        = true;
			s_NewKey[0] = s_NewVal[0] = '\0';
		}

		ImGui::PopStyleVar();
		ImGui::EndChild();
		return;
	}

	auto& reflReg = ReflectionRegistry::Get();
	auto it       = reflReg.NameToClassID.find(doc.BaseTypeName);
	if (it == reflReg.NameToClassID.end())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
		ImGui::TextWrapped("Entity type '%s' not found in registry.", doc.BaseTypeName.c_str());
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
		ImGui::EndChild();
		return;
	}

	ClassID classID = it->second;
	auto compListIt = reflReg.ClassToComponentList.find(classID);
	if (compListIt == reflReg.ClassToComponentList.end())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
		ImGui::TextUnformatted("No components registered.");
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
		ImGui::EndChild();
		return;
	}

	for (ComponentTypeID compID : compListIt->second)
	{
		auto compIt = reflReg.ComponentData.find(compID);
		if (compIt == reflReg.ComponentData.end()) continue;

		const ComponentMetaEx& meta = compIt->second;
		const char* cName           = meta.Name ? meta.Name : "Unknown";
		const auto* fields          = reflReg.GetFields(compID);
		if (!fields || fields->empty()) continue;

		// Component collapsing header with tier badge.
		bool open = ImGui::CollapsingHeader(cName, ImGuiTreeNodeFlags_DefaultOpen);
		ImGui::SameLine();
		TnxWidgets::Tier displayTier;
		switch (meta.TemporalTier)
		{
			case CacheTier::Temporal:
			case CacheTier::Universal: displayTier = TnxWidgets::Tier::Temporal; break;
			case CacheTier::Volatile: displayTier = TnxWidgets::Tier::Volatile; break;
			default: displayTier = TnxWidgets::Tier::Cold; break;
		}
		TnxWidgets::TierBadge(displayTier);

		if (!open) continue;
		ImGui::Indent(8.0f);

		for (const FieldMeta& fm : *fields)
		{
			if (!fm.Name) continue;
			DrawFieldRow(doc, cName, fm.Name, fm.ValueType, fm.RefAssetType, state);
		}

		ImGui::Unindent(8.0f);
		ImGui::Spacing();
	}

	ImGui::PopStyleVar();
	ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Field row
// ---------------------------------------------------------------------------

void PrefabEditorWindow::DrawFieldRow(PrefabDoc& doc, const std::string& compName,
	const std::string& fieldName, FieldValueType valueType,
	AssetType refAssetType, EditorState& state)
{
	// Read current override from JSON.
	const JsonValue* comps     = doc.RootJson.Find("components");
	const JsonValue* compObj   = comps ? comps->Find(compName) : nullptr;
	const JsonValue* fieldJson = compObj ? compObj->Find(fieldName) : nullptr;
	bool hasOverride           = (fieldJson != nullptr);

	ImGui::PushID((compName + "_" + fieldName).c_str());

	// Field label.
	ImGui::PushStyleColor(ImGuiCol_Text,
		hasOverride ? TnxStyle::Color::Fg : TnxStyle::Color::FgMuted);
	ImGui::SetNextItemWidth(90.0f);
	ImGui::TextUnformatted(fieldName.c_str());
	ImGui::PopStyleColor();
	ImGui::SameLine();

	// Reset button (↺) — only shown when override exists.
	if (hasOverride)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, TnxStyle::Color::BgElev);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, TnxStyle::Color::BgElev);
		if (ImGui::SmallButton(("x##rst_" + fieldName).c_str()))
		{
			// Remove the override.
			JsonValue* mComps   = doc.RootJson.Find("components");
			JsonValue* mCompObj = mComps ? mComps->Find(compName) : nullptr;
			if (mCompObj)
			{
				auto& obj = mCompObj->GetObject();
				obj.erase(std::remove_if(obj.begin(), obj.end(),
							  [&](const auto& p)
				{
					return p.first == fieldName;
				}),
					obj.end());
			}
			doc.bDirty                 = true;
			doc.bPreviewRebuildPending = true;
		}
		ImGui::PopStyleColor(4);
		ImGui::SameLine();
	}

	// ── Asset ref field ──────────────────────────────────────────────────
	if (refAssetType != AssetType::Invalid)
	{
		uint32_t currentSlot = 0;
		if (fieldJson && fieldJson->IsNumber())
			currentSlot = static_cast<uint32_t>(fieldJson->AsFloat());

		const char* assetName = MeshManager::Get().GetSlotName(currentSlot);
		const char* preview   = (!assetName || !*assetName) ? "(none)" : assetName;

		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::BeginCombo(("##a_" + fieldName).c_str(), preview))
		{
			uint32_t count = MeshManager::Get().GetMeshCount();
			for (uint32_t i = 0; i < count; ++i)
			{
				const char* n   = MeshManager::Get().GetSlotName(i);
				const char* lbl = (!n || !*n) ? "(unnamed)" : n;
				bool sel        = (i == currentSlot);
				if (ImGui::Selectable(lbl, sel))
				{
					doc.RootJson["components"][compName][fieldName] = JsonValue::Number(i);
					doc.bDirty                                      = true;
					doc.bPreviewRebuildPending                      = true;
				}
				if (sel) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::PopID();
		return;
	}

	// ── Numeric fields ───────────────────────────────────────────────────
	ImGui::SetNextItemWidth(-1.0f);

	switch (valueType)
	{
		case FieldValueType::Float32:
		case FieldValueType::Fixed32:
		case FieldValueType::Float64:
		{
			float fv = fieldJson ? fieldJson->AsFloat() : 0.0f;
			if (!hasOverride)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
				ImGui::PushFont(TnxStyle::Font::MonoRegular);
				ImGui::TextUnformatted("—");
				ImGui::PopFont();
				ImGui::PopStyleColor();
				ImGui::SameLine();
				if (ImGui::SmallButton(("Override##" + fieldName).c_str()))
				{
					doc.RootJson["components"][compName][fieldName] = JsonValue::Number(0.0);
					doc.bDirty                                      = true;
				}
				break;
			}
			if (TnxWidgets::FieldFloat(fieldName.c_str(), &fv))
			{
				doc.RootJson["components"][compName][fieldName] = JsonValue::Number(static_cast<double>(fv));
				doc.bDirty                                      = true;
			}
			if (ImGui::IsItemDeactivatedAfterEdit())
				doc.bPreviewRebuildPending = true;
			break;
		}
		case FieldValueType::Int32:
		{
			int iv = fieldJson ? fieldJson->AsInt() : 0;
			if (!hasOverride)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
				ImGui::TextUnformatted("—");
				ImGui::PopStyleColor();
				ImGui::SameLine();
				if (ImGui::SmallButton(("Override##" + fieldName).c_str()))
				{
					doc.RootJson["components"][compName][fieldName] = JsonValue::Number(0.0);
					doc.bDirty                                      = true;
				}
				break;
			}
			if (ImGui::InputInt(("##i_" + fieldName).c_str(), &iv))
			{
				doc.RootJson["components"][compName][fieldName] = JsonValue::Number(iv);
				doc.bDirty                                      = true;
			}
			if (ImGui::IsItemDeactivatedAfterEdit())
				doc.bPreviewRebuildPending = true;
			break;
		}
		case FieldValueType::Uint32:
		{
			int uv = fieldJson ? fieldJson->AsInt() : 0;
			if (uv < 0) uv = 0;
			if (!hasOverride)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
				ImGui::TextUnformatted("—");
				ImGui::PopStyleColor();
				ImGui::SameLine();
				if (ImGui::SmallButton(("Override##" + fieldName).c_str()))
				{
					doc.RootJson["components"][compName][fieldName] = JsonValue::Number(0.0);
					doc.bDirty                                      = true;
				}
				break;
			}
			if (ImGui::InputInt(("##u_" + fieldName).c_str(), &uv))
			{
				if (uv < 0) uv = 0;
				doc.RootJson["components"][compName][fieldName] = JsonValue::Number(uv);
				doc.bDirty                                      = true;
			}
			if (ImGui::IsItemDeactivatedAfterEdit())
				doc.bPreviewRebuildPending = true;
			break;
		}
		default:
		{
			ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
			ImGui::TextUnformatted("(unsupported type)");
			ImGui::PopStyleColor();
			break;
		}
	}

	ImGui::PopID();
}

// ---------------------------------------------------------------------------
// Preview viewport
// ---------------------------------------------------------------------------

void PrefabEditorWindow::DrawPreviewViewport(PrefabDoc& doc, float availW, float availH,
	EditorState& state)
{
	ImGui::BeginChild("##preview_vp", ImVec2(availW, availH), false,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	bool hovered = ImGui::IsWindowHovered();

	// Orbit on Alt+left-drag.
	if (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::GetIO().KeyAlt)
	{
		ImVec2 delta = ImGui::GetIO().MouseDelta;
		doc.OrbitYaw += delta.x * 0.007f;
		doc.OrbitPitch -= delta.y * 0.007f;
		doc.OrbitPitch = std::clamp(doc.OrbitPitch, -1.45f, 1.45f);
		UpdateOrbitCamera(doc);
	}

	// Zoom on scroll.
	if (hovered)
	{
		float scroll = ImGui::GetIO().MouseWheel;
		if (scroll != 0.0f)
		{
			doc.OrbitDist = std::clamp(doc.OrbitDist - scroll * 0.5f, 0.5f, 50.0f);
			UpdateOrbitCamera(doc);
		}
	}

	// Draw the render target.
	ImVec2 vpSize = ImVec2(availW, availH);
	ImGui::Image(doc.PreviewViewport->ImGuiTexture, vpSize);

	// Hint overlay.
	ImGui::SetCursorPos(ImVec2(8, 8));
	ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgDim);
	ImGui::TextUnformatted("Alt+Drag: Orbit  |  Scroll: Zoom");
	ImGui::PopStyleColor();

	ImGui::EndChild();
}

#endif // TNX_ENABLE_EDITOR
