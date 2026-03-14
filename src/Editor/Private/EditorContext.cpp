#include "EditorContext.h"
#include "EditorPanel.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "Logger.h"

// Panel headers
#include "Panels/WorldOutlinerPanel.h"
#include "Panels/DetailsPanel.h"
#include "Panels/EngineStatsPanel.h"
#include "Panels/LogPanel.h"
#include "Panels/ContentBrowserPanel.h"

EditorContext::EditorContext()  = default;
EditorContext::~EditorContext() = default;

void EditorContext::Initialize(Registry* registry, const EngineConfig* config, LogicThread* logic)
{
	RegistryPtr = registry;
	ConfigPtr   = config;
	LogicPtr    = logic;

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
		ImGui::MenuItem("New Scene", nullptr, false, false);
		ImGui::MenuItem("Open Scene", nullptr, false, false);
		ImGui::MenuItem("Save Scene", nullptr, false, false);
		ImGui::Separator();
		// Exit is not wired — engine shutdown is controlled by Sentinel
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
		ImGui::MenuItem("Play (Standalone)", nullptr, false, false);
		ImGui::MenuItem("Play (Listen Server)", nullptr, false, false);
		ImGui::MenuItem("Play (Client)", nullptr, false, false);
		ImGui::Separator();
		ImGui::MenuItem("Stop", nullptr, false, false);
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Debug"))
	{
		ImGui::MenuItem("Show Demo Window", nullptr, &bShowDemoWindow);
		ImGui::MenuItem("Show ImGui Metrics", nullptr, &bShowMetrics);
		ImGui::EndMenu();
	}

	ImGui::EndMenuBar();
}
