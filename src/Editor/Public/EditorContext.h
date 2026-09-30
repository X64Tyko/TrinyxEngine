#pragma once
#if !defined(TNX_ENABLE_EDITOR)
#error "EditorContext.h requires TNX_ENABLE_EDITOR"
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "AssetDatabase.h"
#include "EditorState.h"
#include "EngineConfig.h"
#include "WorldViewport.h"
#include "UndoCommand.h"
#include "imgui.h"

class ConstructEditorWindow;
class EditorPanel;
class EntityEditorWindow;
class PrefabEditorWindow;
class FlowManagerBase;
class LogicThreadBase;
class ReplicationSystem;
class TrinyxEngine;
class WorldBase;

/// EditorContext — owns all editor UI state and panel drawing.
///
/// Called by the renderer between ImGui::NewFrame() and ImGui::Render().
/// All ImGui:: calls for editor panels live here, keeping editor logic
/// separated from engine rendering code.
class EditorContext
{
public:
	EditorContext();
	~EditorContext();

	void Initialize(TrinyxEngine* engine, LogicThreadBase* logic);

	/// Build the editor UI for this frame.  Called on the render thread
	/// after ImGui::NewFrame(), before ImGui::Render().
	void BuildFrame();

	/// Load a scene file: reset registry, spawn entities, update editor state.
	/// If bReset is false, skips ResetRegistry (used for initial load into an empty world).
	void LoadScene(const std::string& path, bool bReset = true);

	/// Open the Construct editor and create/focus a tab for the given type name.
	/// filePath is the absolute path to the header — when provided the doc is parsed from disk.
	void OpenConstructEditor(const char* typeName, const char* filePath = nullptr);
	/// Open the Entity editor and create/focus a tab for the given type name.
	/// filePath is the absolute path to the header — when provided the doc is parsed from disk.
	void OpenEntityEditor(const char* typeName, const char* filePath = nullptr);

	/// Open the Prefab editor and create/focus a tab for the given .tnxprefab file.
	/// Switches to the Asset workspace automatically.
	void OpenPrefabEditor(const std::string& filePath);

	/// Show the mesh import dialog (called from ContentBrowserPanel).
	void ShowImportDialog()
	{
		bShowImportDialog = true;
		ImportDialogPath.clear();
	}

	/// Handle a file dropped onto the window (called from EditorRenderer on render thread).
	void HandleDroppedFile(const std::string& path);

	/// Spawn a prefab into the current scene. Called from content browser drag-drop or double-click.
	void SpawnPrefab(const std::string& prefabPath);

	/// Delete the currently selected entity (deferred via Spawn handshake).
	void DeleteSelectedEntity();

	/// PIE local mode: single solo world, replaces primary window with game viewport.
	void StartPIELocal();
	/// PIE networked mode: server + client worlds in floating viewports.
	void StartPIE();
	/// Stop whichever PIE mode is active.
	void StopPIE();
	bool IsPIEActive() const { return bPIEActive; }
	bool bPIEStopRequested = false; // Set by BuildFrame (Escape), consumed after ImGui::Render

	/// Register a panel. EditorContext takes ownership.
	template <typename T, typename... Args>
	T* AddPanel(Args&&... args)
	{
		auto panel = std::make_unique<T>(std::forward<Args>(args)...);
		T* ptr     = panel.get();
		Panels.push_back(std::move(panel));
		return ptr;
	}

	/// Returns the screen-space top-left of the 3D viewport panel (logical pixels).
	/// Updated each frame during DrawEditorViewportPanel(); valid after BuildFrame() returns.
	ImVec2 GetViewportPanelPos() const { return ViewportPanelPos; }

	std::vector<std::unique_ptr<UndoCommand>> UndoStack;
	size_t UndoIndex                = 0;
	static constexpr size_t MaxUndo = 50;

	void PushCommand(std::unique_ptr<UndoCommand> cmd);
	void Undo();
	void Redo();
	bool CanUndo() const { return UndoIndex > 0; }
	bool CanRedo() const { return UndoIndex < UndoStack.size(); }
	EditorState& GetState() { return State; }

private:
	void BuildDockspace();
	void BuildMenuBar();
	void ApplyDefaultLayout(unsigned int dockspaceID);

	TrinyxEngine* EnginePtr                   = nullptr;
	LogicThreadBase* LogicPtr                 = nullptr;
	ConstructEditorWindow* ConstructEditorPtr = nullptr;
	EntityEditorWindow* EntityEditorPtr       = nullptr;
	PrefabEditorWindow* PrefabEditorPtr       = nullptr;
	EditorPanel* OutlinerPanelPtr             = nullptr;
	EditorPanel* DetailsPanelPtr              = nullptr;

	EditorState State;
	AssetDatabase AssetDB;
	std::vector<std::unique_ptr<EditorPanel>> Panels;

	void DrawFileDialog();
	void DrawImportDialog();
	void DrawUnsavedWarning();
	void DrawAssetIssuesDialog();
	void DrawGizmo();
	void ConsumePick();

	void CheckForAssetIssues();
	void LoadEditorSettings();
	void SaveEditorSettings();

	struct AssetIssue
	{
		AssetID ID;
		std::string Name;
		std::string RegisteredPath;
		std::string SuggestedSourcePath; // pre-filled reimport path, user-editable
		char ReimportBuf[512]  = {};
		bool ShowReimportInput = false;
	};
	std::vector<AssetIssue> AssetIssues;
	bool bShowAssetIssuesDialog = false;

	/// Import a glTF/glb file: convert to .tnxmesh in content/, register with
	/// AssetDatabase and MeshManager. Returns the mesh slot or UINT32_MAX on failure.
	uint32_t ImportMeshAsset(const std::string& gltfPath);

	// --- Play/Stop scene snapshot ---
	// On Play: snapshot all field data so Stop can restore it.
	// On Stop: write snapshot back into the slab (sim is paused, safe to write directly).
	void SnapshotScene();
	void RestoreSnapshot();

	// Per-archetype snapshot: field data for all chunks + entity count at snapshot time.
	struct ArchetypeSnapshot
	{
		ClassID ArchClassID;
		uint32_t TotalEntityCount; // Entity count at snapshot time

		// Per-chunk field data: [chunk0 fields][chunk1 fields]...
		// Each chunk block is: [field0 * entityCount][field1 * entityCount]...
		struct ChunkData
		{
			void* Chunk;          // Pointer identity (not ownership)
			uint32_t EntityCount; // Per-chunk count at snapshot time
			std::vector<uint8_t> FieldData;
		};

		std::vector<ChunkData> Chunks;
	};

	std::vector<ArchetypeSnapshot> PlaySnapshot;
	bool bHasSnapshot = false;

	// --- PIE local (single solo world, fullscreen) ---
	std::unique_ptr<FlowManagerBase> LocalPIEFlow;
	std::unique_ptr<WorldViewport> LocalPIEViewport;
	EngineConfig LocalPIEConfig;
	bool bPIELocalMode = false;

	// --- PIE networked (multi-world, floating viewports) ---
	// Server may be headless (no viewport) or rendered.
	// N clients each get their own World + viewport.
	struct PIEClient
	{
		std::unique_ptr<FlowManagerBase> Flow;
		std::unique_ptr<WorldViewport> Viewport;
		EngineConfig Config;       // Client-mode config (game config + Mode=Client)
		uint32_t ClientHandle = 0; // Client-side GNS connection handle (outgoing)
		uint32_t ServerHandle = 0; // Server-side accepted handle (for replication)
	};

	std::unique_ptr<FlowManagerBase> ServerFlow;
	std::unique_ptr<WorldViewport> ServerViewport; // nullptr if headless
	std::unique_ptr<ReplicationSystem> Replicator;
	EngineConfig ServerConfig; // Server-mode config (game config + Mode=Server/Host)
	std::vector<PIEClient> PIEClients;
	bool bPIEActive          = false;
	bool bPIEPaused          = false;
	bool bPrePIESimWasPaused = true; // Editor sim paused state before PIE — restored on StopPIE
	enum class PIEMode : uint8_t
	{
		Local,
		ListenServer,
		HeadlessServer
	};
	PIEMode CurrentPIEMode = PIEMode::Local;
	bool bServerVisible    = true; // derived from CurrentPIEMode before StartPIE()
	int PIEClientCount     = 1;    // Number of client worlds to spawn in PIE

	void DrawEditorViewportPanel();
	void DrawEditorGrid();
	void DrawViewportPanel(const char* title, WorldViewport& vp);

	enum class PendingActionType : uint8_t
	{
		None,
		OpenScene
	};

	// --- Workspace switcher ---
	enum class Workspace : uint8_t
	{
		Layout,
		Logic,
		Simulate,
		Network,
		Profile,
		Asset,
		COUNT
	};
	Workspace CurrentWorkspace                                     = Workspace::Layout;
	Workspace LastAppliedWorkspace                                 = Workspace::COUNT; // sentinel: COUNT means "nothing applied yet"
	bool bWorkspaceLayoutBuilt[static_cast<int>(Workspace::COUNT)] = {};
	void ApplyWorkspaceLayout(unsigned int dockspaceID, Workspace ws);
	bool IsInWorkspace(const char* windowName) const;
	std::vector<std::string> WorkspaceWindows; ///< Windows the current workspace docks.
	unsigned int WorkspaceMainNode = 0;        ///< Dock node holding the Viewport; fallback for other windows.

	// --- Frame budget overlay (bottom-right corner, always visible) ---
	void DrawFrameBudgetOverlay();
	void UpdateEditorCamera(bool engineGetsInput);

	bool bMouseReleasedDuringPlay = false;
	bool bEditorCameraFlying      = false; ///< RMB fly on the edit viewport's own camera.
	bool bShowDemoWindow          = false;
	bool bShowMetrics             = false;
	bool bFirstFrame              = true;
	bool bShowFileDialog          = false;
	bool bFileDialogForSave       = false;
	bool bShowUnsavedWarning      = false;
	bool bShowImportDialog        = false;
	void DrawPrefabSaveDialog();
	bool bShowPrefabSaveDialog      = false;
	bool ViewportPanelHovered       = false;
	ImVec2 ViewportPanelPos         = { 0, 0 };
	ImVec2 ViewportPanelSize        = { 0, 0 };
	PendingActionType PendingAction = PendingActionType::None;
	std::string FileDialogPath;
	std::string ImportDialogPath;
};
