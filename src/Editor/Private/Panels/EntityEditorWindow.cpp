#include "Panels/EntityEditorWindow.h"
#if !defined(TNX_ENABLE_EDITOR)
#error "EntityEditorWindow.cpp requires TNX_ENABLE_EDITOR"
#endif

#include "EditorState.h"
#include "EngineConfig.h"
#include "TnxStyle.h"
#include "TnxWidgets.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

// ---------------------------------------------------------------------------
// File parser — populates Components and seeds the spawn graph from an existing header
// ---------------------------------------------------------------------------

static void ParseEntityFile(const char* filePath, EntityDoc& doc)
{
	std::ifstream f(filePath, std::ios::binary);
	if (!f.is_open()) return;
	std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

	// Find all C-prefixed component member declarations: CXxx<WIDTH> MemberName
	// We search for the literal "<WIDTH>" which only appears as a single-arg template
	// instantiation on component types (CScale<WIDTH>, CColor<WIDTH>, etc.).
	const std::string widthToken = "<WIDTH>";
	size_t pos                   = 0;
	while ((pos = src.find(widthToken, pos)) != std::string::npos)
	{
		// Walk backward from pos to extract the type name immediately before <WIDTH>
		size_t wordEnd   = pos;
		size_t wordStart = wordEnd;
		while (wordStart > 0 && (std::isalnum((unsigned char)src[wordStart - 1]) || src[wordStart - 1] == '_'))
			--wordStart;

		const std::string typeName = src.substr(wordStart, wordEnd - wordStart);

		// Only count it if it follows the CXxx component naming convention
		if (!typeName.empty() && typeName[0] == 'C' && std::isupper((unsigned char)typeName[1]))
		{
			// Confirm there is a member name after the closing >
			size_t afterClose = pos + widthToken.size();
			while (afterClose < src.size() && std::isspace((unsigned char)src[afterClose]))
				++afterClose;
			size_t nameStart = afterClose;
			size_t nameEnd   = nameStart;
			while (nameEnd < src.size() && (std::isalnum((unsigned char)src[nameEnd]) || src[nameEnd] == '_'))
				++nameEnd;

			if (nameEnd > nameStart)
			{
				EntityDoc::ComponentEntry entry = {};
				strncpy(entry.TypeName, typeName.c_str(), sizeof(entry.TypeName) - 1);
				doc.Components.push_back(entry);
			}
		}

		pos += widthToken.size();
	}

	doc.SpawnGraph.Clear();
	doc.SpawnGraph.AddEventNode(NodeGraphCanvas::NodeKind::Event_OnSpawn, 80.0f, 120.0f);
}

// ---------------------------------------------------------------------------
// EntityDoc
// ---------------------------------------------------------------------------

EntityDoc::EntityDoc(const char* typeName, const char* filePath)
	: TypeName(typeName)
{
	SpawnGraph.Clear();

	if (filePath && filePath[0])
	{
		FilePath = filePath;
		strncpy(OutputPath, filePath, sizeof(OutputPath) - 1);
		ParseEntityFile(filePath, *this);
	}
	else
	{
		SpawnGraph.AddEventNode(NodeGraphCanvas::NodeKind::Event_OnSpawn, 80.0f, 120.0f);
	}
}

// ---------------------------------------------------------------------------
// EntityEditorWindow
// ---------------------------------------------------------------------------

EntityEditorWindow::EntityEditorWindow()
	: EditorPanel("Entity Editor")
{
}

void EntityEditorWindow::OpenEntity(const char* typeName, const char* filePath)
{
	for (int i = 0; i < static_cast<int>(Docs.size()); ++i)
	{
		if (Docs[i].TypeName == typeName)
		{
			ActiveTab = i;
			bVisible  = true;
			return;
		}
	}
	Docs.emplace_back(typeName, filePath);
	ActiveTab = static_cast<int>(Docs.size()) - 1;
	bVisible  = true;
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

void EntityEditorWindow::Draw(EditorState& state)
{
	if (!BeginPadded(ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		ImGui::End();
		return;
	}

	TnxWidgets::PanelHeader(nullptr, "Entity Editor");

	// --- Top toolbar ---
	if (ImGui::Button("+ New"))
		bShowNewDialog = true;

	if (bShowNewDialog)
	{
		ImGui::SameLine();
		ImGui::SetNextItemWidth(160.0f);
		ImGui::InputText("##NewEName", NewTypeBuf, sizeof(NewTypeBuf));
		ImGui::SameLine();
		if (ImGui::Button("Create"))
		{
			if (NewTypeBuf[0] != '\0')
			{
				OpenEntity(NewTypeBuf);
				bShowNewDialog = false;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) bShowNewDialog = false;
	}

	if (Docs.empty())
	{
		ImGui::Separator();
		ImGui::TextDisabled("No Entity types open. Click '+ New' or open one from the Content Browser.");
		ImGui::End();
		return;
	}

	// --- Tab bar ---
	DrawTabBar();

	if (ActiveTab < 0 || ActiveTab >= static_cast<int>(Docs.size()))
	{
		ImGui::End();
		return;
	}

	EntityDoc& doc = Docs[static_cast<size_t>(ActiveTab)];

	// Pane toggle buttons — right-aligned before the separator.
	{
		float spacing   = ImGui::GetStyle().ItemSpacing.x;
		float btnW      = ImGui::CalcTextSize("[ Comp ]").x + ImGui::GetStyle().FramePadding.x * 2.0f;
		float totalBtns = btnW * 2.0f + spacing;
		ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - totalBtns);
		if (ImGui::SmallButton(bLeftPaneVisible ? "[ Comp ]" : "[ > ]")) bLeftPaneVisible = !bLeftPaneVisible;
		ImGui::SameLine();
		if (ImGui::SmallButton(bRightPaneVisible ? "[ Info ]" : "[ < ]")) bRightPaneVisible = !bRightPaneVisible;
	}

	ImGui::Separator();

	// --- Three-column layout: Components | Spawn Graph | Info ---
	float totalH = ImGui::GetContentRegionAvail().y;

	if (bLeftPaneVisible)
	{
		DrawComponentPanel(doc, totalH);
		ImGui::SameLine();
	}
	DrawSpawnGraph(doc, 0.0f, totalH);
	if (bRightPaneVisible)
	{
		ImGui::SameLine();
		DrawInfoPanel(doc, totalH, state);
	}

	ImGui::End();
}

void EntityEditorWindow::DrawTabBar()
{
	if (ImGui::BeginTabBar("##EntityTabs"))
	{
		for (int i = 0; i < static_cast<int>(Docs.size()); ++i)
		{
			bool open               = true;
			ImGuiTabItemFlags flags = (i == ActiveTab) ? ImGuiTabItemFlags_SetSelected : 0;
			if (ImGui::BeginTabItem(Docs[static_cast<size_t>(i)].TypeName.c_str(), &open, flags))
			{
				ActiveTab = i;
				ImGui::EndTabItem();
			}
			if (!open)
			{
				Docs.erase(Docs.begin() + i);
				if (ActiveTab >= static_cast<int>(Docs.size())) ActiveTab = static_cast<int>(Docs.size()) - 1;
				break;
			}
		}
		ImGui::EndTabBar();
	}
}

void EntityEditorWindow::DrawComponentPanel(EntityDoc& doc, float height)
{
	ImGui::BeginChild("##EntityComp", ImVec2(180.0f, height), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);

	ImGui::TextDisabled("Components");
	ImGui::SameLine();
	if (ImGui::SmallButton("+##AddComp"))
	{
		EntityDoc::ComponentEntry entry;
		doc.Components.push_back(entry);
	}
	ImGui::Separator();

	for (int i = 0; i < static_cast<int>(doc.Components.size()); ++i)
	{
		auto& c = doc.Components[static_cast<size_t>(i)];
		ImGui::PushID(i);

		ImGui::SetNextItemWidth(130.0f);
		ImGui::InputText("##CType", c.TypeName, sizeof(c.TypeName));
		ImGui::SameLine();
		if (ImGui::SmallButton("x"))
		{
			doc.Components.erase(doc.Components.begin() + i);
			ImGui::PopID();
			break;
		}

		ImGui::PopID();
	}

	ImGui::EndChild();
}

void EntityEditorWindow::DrawSpawnGraph(EntityDoc& doc, float /*width*/, float height)
{
	ImGui::BeginChild("##EntityCenter", ImVec2(0.0f, height), ImGuiChildFlags_ResizeX,
		ImGuiWindowFlags_NoScrollbar);

	ImGui::TextDisabled("Spawn Initializer");
	ImGui::Separator();

	float graphH = ImGui::GetContentRegionAvail().y;
	float graphW = ImGui::GetContentRegionAvail().x;

	char canvasID[64];
	snprintf(canvasID, sizeof(canvasID), "ee_%p", (void*)&doc);

	doc.SpawnGraph.DrawCanvas(canvasID, graphW, graphH);

	ImGui::EndChild();
}

void EntityEditorWindow::DrawInfoPanel(EntityDoc& doc, float height, EditorState& state)
{
	ImGui::BeginChild("##EntityInfo", ImVec2(0.0f, height), ImGuiChildFlags_Borders);

	ImGui::TextDisabled("Type");
	ImGui::TextUnformatted(doc.TypeName.c_str());
	ImGui::Spacing();

	ImGui::TextDisabled("Output Path");
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputText("##OutPathEE", doc.OutputPath, sizeof(doc.OutputPath));

	ImGui::Spacing();
	if (ImGui::Button("Export .h##EEExp", ImVec2(-1.0f, 0.0f)))
	{
		// TODO: code generation
		snprintf(doc.StatusMsg, sizeof(doc.StatusMsg), "Code gen not yet implemented.");
		doc.bStatusError = true;
	}

	if (doc.StatusMsg[0] != '\0')
	{
		ImVec4 col = doc.bStatusError ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f) : ImVec4(0.35f, 1.0f, 0.35f, 1.0f);
		ImGui::TextColored(col, "%s", doc.StatusMsg);
	}

	ImGui::Spacing();
	ImGui::TextDisabled("Components");
	for (const auto& c : doc.Components)
		ImGui::Text("  %s", c.TypeName);

	ImGui::EndChild();
}
