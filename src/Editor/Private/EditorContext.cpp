#include "EditorContext.h"
#include "EditorPanel.h"
#include "EntityBuilder.h"
#include "TrinyxEngine.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "Logger.h"
#include "LogicThread.h"
#include "Registry.h"

// Panel headers
#include "Panels/WorldOutlinerPanel.h"
#include "Panels/DetailsPanel.h"
#include "Panels/EngineStatsPanel.h"
#include "Panels/LogPanel.h"
#include "Panels/ContentBrowserPanel.h"

EditorContext::EditorContext()  = default;
EditorContext::~EditorContext() = default;

void EditorContext::Initialize(TrinyxEngine* engine, LogicThread* logic)
{
	EnginePtr = engine;
	LogicPtr  = logic;

	// Populate shared state pointers for panels
	State.EnginePtr   = engine;
	State.RegistryPtr = engine->GetRegistry();
	State.ConfigPtr   = engine->GetConfig();
	State.LogicPtr    = logic;

	// Initialize asset database from project content directory
	const EngineConfig* cfg = engine->GetConfig();
	if (cfg->ProjectDir[0] != '\0')
	{
		std::string contentDir = std::string(cfg->ProjectDir) + "/content";
		AssetDB.Initialize(contentDir.c_str());
		State.AssetDB = &AssetDB;
	}

	// Register all panels
	AddPanel<WorldOutlinerPanel>();
	AddPanel<DetailsPanel>();
	AddPanel<EngineStatsPanel>();
	AddPanel<LogPanel>();
	AddPanel<ContentBrowserPanel>();

	LOG_INFO_F("[Editor] Initialized with %zu panels", Panels.size());
}

void EditorContext::BuildFrame()
{
	BuildDockspace();

	// Draw all panels
	for (auto& panel : Panels)
	{
		panel->Tick(State);
	}

	// Modals / overlays
	DrawFileDialog();
	DrawUnsavedWarning();

	// Debug windows
	if (bShowDemoWindow) ImGui::ShowDemoWindow(&bShowDemoWindow);
	if (bShowMetrics) ImGui::ShowMetricsWindow(&bShowMetrics);
}

void EditorContext::BuildDockspace()
{
	// Full-viewport dockspace with menu bar
	ImGuiWindowFlags windowFlags = ImGuiWindowFlags_MenuBar
		| ImGuiWindowFlags_NoDocking
		| ImGuiWindowFlags_NoTitleBar
		| ImGuiWindowFlags_NoCollapse
		| ImGuiWindowFlags_NoResize
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoBringToFrontOnFocus
		| ImGuiWindowFlags_NoNavFocus
		| ImGuiWindowFlags_NoBackground;

	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	ImGui::SetNextWindowViewport(viewport->ID);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

	ImGui::Begin("EditorDockspace", nullptr, windowFlags);
	ImGui::PopStyleVar(3);

	ImGuiID dockspaceID = ImGui::GetID("EditorDockspaceID");
	ImGui::DockSpace(dockspaceID, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

	// Apply default layout on first frame
	if (bFirstFrame)
	{
		ApplyDefaultLayout(dockspaceID);
		bFirstFrame = false;
	}

	BuildMenuBar();

	ImGui::End();
}

void EditorContext::ApplyDefaultLayout(unsigned int dockspaceID)
{
	ImGui::DockBuilderRemoveNode(dockspaceID);
	ImGui::DockBuilderAddNode(dockspaceID, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(dockspaceID, ImGui::GetMainViewport()->WorkSize);

	// Split: bottom 25% for tabbed panels, top 75% for main area
	ImGuiID top, bottom;
	ImGui::DockBuilderSplitNode(dockspaceID, ImGuiDir_Down, 0.25f, &bottom, &top);

	// Split top: left 15% for outliner, remainder for center+right
	ImGuiID left, centerRight;
	ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, 0.15f, &left, &centerRight);

	// Split center+right: right 25% for details, center for viewport
	ImGuiID right, center;
	ImGui::DockBuilderSplitNode(centerRight, ImGuiDir_Right, 0.25f, &right, &center);

	// Dock panels
	ImGui::DockBuilderDockWindow("World Outliner", left);
	ImGui::DockBuilderDockWindow("Details", right);

	// Bottom: tabbed — Content Browser, Log, Engine Stats
	ImGui::DockBuilderDockWindow("Content Browser", bottom);
	ImGui::DockBuilderDockWindow("Log", bottom);
	ImGui::DockBuilderDockWindow("Engine Stats", bottom);

	// Mark center node as the viewport (passthru so the 3D scene shows through)
	ImGuiDockNode* centerNode = ImGui::DockBuilderGetNode(center);
	if (centerNode)
	{
		centerNode->LocalFlags |= ImGuiDockNodeFlags_CentralNode;
	}

	ImGui::DockBuilderFinish(dockspaceID);
}

void EditorContext::BuildMenuBar()
{
	if (!ImGui::BeginMenuBar()) return;

	if (ImGui::BeginMenu("File"))
	{
		if (ImGui::MenuItem("Open Scene...", "Ctrl+O"))
		{
			if (State.bSceneDirty)
			{
				bShowUnsavedWarning = true;
				PendingAction       = PendingActionType::OpenScene;
			}
			else
			{
				bShowFileDialog    = true;
				bFileDialogForSave = false;
				FileDialogPath     = State.CurrentScenePath;
			}
		}
		if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, !State.CurrentScenePath.empty()))
		{
			EntityBuilder::SaveToFile(State.RegistryPtr, State.CurrentSceneName.c_str(),
									  State.CurrentScenePath.c_str());
			State.bSceneDirty = false;
		}
		if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S"))
		{
			bShowFileDialog    = true;
			bFileDialogForSave = true;
			FileDialogPath     = State.CurrentScenePath;
		}
		ImGui::Separator();
		ImGui::MenuItem("Exit", nullptr, false, false);
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Edit"))
	{
		ImGui::MenuItem("Undo", "Ctrl+Z", false, false);
		ImGui::MenuItem("Redo", "Ctrl+Y", false, false);
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("View"))
	{
		for (auto& panel : Panels)
		{
			ImGui::MenuItem(panel->Title, nullptr, &panel->bVisible);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Reset Layout"))
		{
			bFirstFrame = true;
		}
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Play"))
	{
		bool simulating = LogicPtr && !LogicPtr->IsSimPaused();

		if (ImGui::MenuItem("Play", nullptr, false, !simulating))
		{
			if (LogicPtr) LogicPtr->SetSimPaused(false);
		}
		if (ImGui::MenuItem("Stop", nullptr, false, simulating))
		{
			if (LogicPtr) LogicPtr->SetSimPaused(true);
		}
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Debug"))
	{
		ImGui::MenuItem("Show Demo Window", nullptr, &bShowDemoWindow);
		ImGui::MenuItem("Show ImGui Metrics", nullptr, &bShowMetrics);
		ImGui::EndMenu();
	}

	// Scene name indicator (right-aligned in menu bar)
	{
		char sceneLabel[256];
		snprintf(sceneLabel, sizeof(sceneLabel), "%s%s",
				 State.bSceneDirty ? "* " : "",
				 State.CurrentSceneName.c_str());

		float textWidth = ImGui::CalcTextSize(sceneLabel).x;
		float available = ImGui::GetContentRegionAvail().x;
		if (available > textWidth) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - textWidth);

		ImGui::TextDisabled("%s", sceneLabel);
	}

	ImGui::EndMenuBar();
}

void EditorContext::DrawFileDialog()
{
	if (!bShowFileDialog) return;

	ImGui::OpenPopup("Scene File");

	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(500, 120), ImGuiCond_Appearing);

	if (ImGui::BeginPopupModal("Scene File", &bShowFileDialog, ImGuiWindowFlags_AlwaysAutoResize))
	{
		char pathBuf[512];
		snprintf(pathBuf, sizeof(pathBuf), "%s", FileDialogPath.c_str());

		ImGui::Text(bFileDialogForSave ? "Save scene to:" : "Open scene from:");
		ImGui::SetNextItemWidth(-1);
		ImGui::InputText("##path", pathBuf, sizeof(pathBuf));
		FileDialogPath = pathBuf;

		ImGui::Separator();

		if (ImGui::Button(bFileDialogForSave ? "Save" : "Open", ImVec2(120, 0)))
		{
			if (bFileDialogForSave)
			{
				// Extract scene name from path
				std::string name = FileDialogPath;
				size_t lastSlash = name.find_last_of('/');
				if (lastSlash != std::string::npos) name = name.substr(lastSlash + 1);
				size_t dot = name.find_last_of('.');
				if (dot != std::string::npos) name = name.substr(0, dot);

				EntityBuilder::SaveToFile(State.RegistryPtr, name.c_str(), FileDialogPath.c_str());
				State.CurrentScenePath = FileDialogPath;
				State.CurrentSceneName = name;
				State.bSceneDirty      = false;
			}
			else
			{
				// Clear existing scene, then load new one through spawn handshake
				std::string path = FileDialogPath;
				EnginePtr->Spawn([path](Registry* reg)
				{
					reg->ResetRegistry();
					EntityBuilder::SpawnFromFile(reg, path.c_str());
				});

				std::string name = FileDialogPath;
				size_t lastSlash = name.find_last_of('/');
				if (lastSlash != std::string::npos) name = name.substr(lastSlash + 1);
				size_t dot = name.find_last_of('.');
				if (dot != std::string::npos) name = name.substr(0, dot);

				State.CurrentScenePath = FileDialogPath;
				State.CurrentSceneName = name;
				State.bSceneDirty      = false;
			}

			bShowFileDialog = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0)))
		{
			bShowFileDialog = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}

void EditorContext::DrawUnsavedWarning()
{
	if (!bShowUnsavedWarning) return;

	ImGui::OpenPopup("Unsaved Changes");

	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

	if (ImGui::BeginPopupModal("Unsaved Changes", &bShowUnsavedWarning, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("Scene \"%s\" has unsaved changes.", State.CurrentSceneName.c_str());
		ImGui::Text("Do you want to save before continuing?");
		ImGui::Separator();

		if (ImGui::Button("Save", ImVec2(100, 0)))
		{
			if (!State.CurrentScenePath.empty())
			{
				EntityBuilder::SaveToFile(State.RegistryPtr, State.CurrentSceneName.c_str(),
										  State.CurrentScenePath.c_str());
				State.bSceneDirty = false;
			}

			// Proceed with pending action
			if (PendingAction == PendingActionType::OpenScene)
			{
				bShowFileDialog    = true;
				bFileDialogForSave = false;
				FileDialogPath     = State.CurrentScenePath;
			}

			bShowUnsavedWarning = false;
			PendingAction       = PendingActionType::None;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();
		if (ImGui::Button("Discard", ImVec2(100, 0)))
		{
			State.bSceneDirty = false;

			if (PendingAction == PendingActionType::OpenScene)
			{
				bShowFileDialog    = true;
				bFileDialogForSave = false;
				FileDialogPath     = State.CurrentScenePath;
			}

			bShowUnsavedWarning = false;
			PendingAction       = PendingActionType::None;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(100, 0)))
		{
			bShowUnsavedWarning = false;
			PendingAction       = PendingActionType::None;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}
