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
#include "imgui.h"

class AudioManager;
class EditorPanel;
class FlowManager;
class LogicThreadBase;
class MeshManager;
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

	void Initialize(TrinyxEngine* engine, LogicThreadBase* logic, MeshManager* meshMgr);

	/// Build the editor UI for this frame.  Called on the render thread
	/// after ImGui::NewFrame(), before ImGui::Render().
	void BuildFrame();

	/// Load a scene file: reset registry, spawn entities, update editor state.
	/// If bReset is false, skips ResetRegistry (used for initial load into an empty world).
	void LoadScene(const std::string& path, bool bReset = true);

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

	/// PIE (Play-In-Editor) networked mode: server + client worlds.
	void StartPIE();
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

private:
	void BuildDockspace();
	void BuildMenuBar();
	void ApplyDefaultLayout(unsigned int dockspaceID);

	TrinyxEngine* EnginePtr = nullptr;
	LogicThreadBase* LogicPtr   = nullptr;

	EditorState State;
	AssetDatabase AssetDB;
	std::vector<std::unique_ptr<EditorPanel>> Panels;

	void DrawFileDialog();
	void DrawImportDialog();
	void DrawUnsavedWarning();
	void DrawGizmo();
	void ConsumePick();

	/// Import a glTF/glb file: convert to .tnxmesh in content/, register with
	/// AssetDatabase and MeshManager. Returns the mesh slot or UINT32_MAX on failure.
	uint32_t ImportMeshAsset(const std::string& gltfPath);

	/// Load all .tnxmesh files from AssetDatabase into MeshManager at startup.
	void LoadAllMeshAssets();

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

	// --- PIE (networked multi-world) ---
	// Server may be headless (no viewport) or rendered.
	// N clients each get their own World + viewport.
	struct PIEClient
	{
		std::unique_ptr<FlowManager> Flow;
		std::unique_ptr<WorldViewport> Viewport;
		EngineConfig Config;       // Client-mode config (game config + Mode=Client)
		uint32_t ClientHandle = 0; // Client-side GNS connection handle (outgoing)
		uint32_t ServerHandle = 0; // Server-side accepted handle (for replication)
	};

	std::unique_ptr<FlowManager> ServerFlow;
	std::unique_ptr<WorldViewport> ServerViewport; // nullptr if headless
	std::unique_ptr<ReplicationSystem> Replicator;
	EngineConfig ServerConfig; // Server-mode config (game config + Mode=Server/Host)
	std::vector<PIEClient> PIEClients;
	bool bPIEActive          = false;
	bool bPrePIESimWasPaused = true; // Editor sim paused state before PIE — restored on StopPIE
	bool bServerVisible = true; // false = headless server (no viewport)
	int PIEClientCount  = 1;    // Number of client worlds to spawn in PIE

	void DrawEditorViewportPanel();
	void DrawViewportPanel(const char* title, WorldViewport& vp);

	enum class PendingActionType : uint8_t { None, OpenScene };

	bool bMouseReleasedDuringPlay   = false;
	bool bShowDemoWindow            = false;
	bool bShowMetrics               = false;
	bool bFirstFrame                = true;
	bool bShowFileDialog            = false;
	bool bFileDialogForSave         = false;
	bool bShowUnsavedWarning        = false;
	bool bShowImportDialog          = false;
	bool ViewportPanelHovered       = false;
	ImVec2 ViewportPanelPos         = {0, 0};
	ImVec2 ViewportPanelSize        = {0, 0};
	PendingActionType PendingAction = PendingActionType::None;
	std::string FileDialogPath;
	std::string ImportDialogPath;

	MeshManager* MeshMgr   = nullptr;
	AudioManager* AudioMgr = nullptr;
};
