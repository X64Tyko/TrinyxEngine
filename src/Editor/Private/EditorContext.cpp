#include "EditorContext.h"
#include "EditorPanel.h"
#include "UndoCommand.h"
#include "EntityBuilder.h"
#include "FlowManager.h"
#include "Globals.h"
#include "Json.h"
#include "ReflectionRegistry.h"
#include "JoltPhysics.h"
#include "TrinyxEngine.h"
#include "World.h"
#include "WorldBase.h"
#include "EditorRenderer.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include "Logger.h"
#include "LogicThreadBase.h"
#include "AudioAsset.h"
#include "AudioManager.h"
#include "MeshAsset.h"
#include "MeshImporter.h"
#include "MeshManager.h"
#include "Registry.h"
#include "CacheSlotMeta.h"
#include "NetConnectionManager.h"
#include "PIENetThread.h"
#include "ReplicationSystem.h"
#include "TemporalComponentCache.h"
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <SDL3/SDL.h>

// Panel headers
#include "Panels/WorldOutlinerPanel.h"
#include "Panels/DetailsPanel.h"
#include "Panels/EngineStatsPanel.h"
#include "Panels/LogPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/NodeScriptPanel.h"
#include "Panels/ComponentGeneratorPanel.h"

EditorContext::EditorContext() = default;

EditorContext::~EditorContext()
{
	if (bPIEActive) StopPIE();
}

void EditorContext::Initialize(TrinyxEngine* engine, LogicThreadBase* logic, MeshManager* meshMgr)
{
	EnginePtr = engine;
	LogicPtr  = logic;
	MeshMgr   = meshMgr;
#ifndef TNX_HEADLESS
	AudioMgr = engine->GetAudio();
#endif

	// Populate shared state pointers for panels
	State.EnginePtr   = engine;
	State.RegistryPtr = engine->GetRegistry();
	State.ConfigPtr   = engine->GetConfig();
	State.LogicPtr    = logic;
	State.EditorCtx   = this;
	State.MeshMgrPtr  = meshMgr;

	// Initialize asset database from project content directory
	const EngineConfig* cfg = engine->GetConfig();
	if (cfg->ProjectDir[0] != '\0')
	{
		std::string contentDir = std::string(cfg->ProjectDir) + "/content";
		AssetDB.Initialize(contentDir.c_str());
		State.AssetDB = &AssetDB;

		// Load all existing .tnxmesh files into MeshManager
		LoadAllMeshAssets();
	}

	// Load default scene if configured
	if (cfg->DefaultScene[0] != '\0' && cfg->ProjectDir[0] != '\0')
	{
		std::string scenePath = std::string(cfg->ProjectDir) + "/content/" + cfg->DefaultScene;
		LoadScene(scenePath, false);
	}

	// Force one logic tick so the renderer gets a valid initial transform snapshot.
	if (LogicPtr && !LogicPtr->IsRunning())
	{
		LogicPtr->TickOnce();
	}

	// Register all panels
	AddPanel<WorldOutlinerPanel>();
	AddPanel<DetailsPanel>();
	AddPanel<EngineStatsPanel>();
	AddPanel<LogPanel>();
	AddPanel<ContentBrowserPanel>();
	AddPanel<NodeScriptPanel>();
	AddPanel<ComponentGeneratorPanel>();

	LOG_ENG_INFO_F("[Editor] Initialized with %zu panels", Panels.size());
}

void EditorContext::LoadScene(const std::string& path, bool bReset)
{
	// Read file and parse JSON so we can extract metadata before spawning
	std::ifstream file(path);
	if (!file.is_open())
	{
		LOG_ENG_ERROR_F("[Editor] Failed to open scene '%s'", path.c_str());
		return;
	}
	std::ostringstream ss;
	ss << file.rdbuf();
	JsonValue root = JsonParse(ss.str());
	if (root.IsNull())
	{
		LOG_ENG_ERROR_F("[Editor] Failed to parse JSON from '%s'", path.c_str());
		return;
	}

	// Extract scene metadata (defaultState, defaultMode)
	auto meta = EntityBuilder::ParseSceneMeta(root);

	// Spawn entities via handshake
	Registry* spawnReg = EnginePtr->GetDefaultWorld() ? EnginePtr->GetDefaultWorld()->GetRegistry() : nullptr;
	JsonValue* rootPtr = &root;
	EnginePtr->Spawn([spawnReg, rootPtr, bReset](uint32_t)
	{
		if (bReset) spawnReg->ResetRegistry();
		// Detect format: prefab vs scene
		if (rootPtr->Find("entities")) EntityBuilder::SpawnScene(spawnReg, *rootPtr);
		else EntityBuilder::SpawnEntity(spawnReg, *rootPtr);
	});

	State.CurrentScenePath  = path;
	State.CurrentSceneName  = meta.Name.empty() ? path : meta.Name;
	State.SceneDefaultState = meta.DefaultState;
	State.SceneDefaultMode  = meta.DefaultMode;

	// Fallback: derive name from filename if not in metadata
	if (meta.Name.empty())
	{
		size_t lastSlash = State.CurrentSceneName.find_last_of('/');
		if (lastSlash != std::string::npos) State.CurrentSceneName = State.CurrentSceneName.substr(lastSlash + 1);
		size_t dot = State.CurrentSceneName.find_last_of('.');
		if (dot != std::string::npos) State.CurrentSceneName = State.CurrentSceneName.substr(0, dot);
	}

	State.bSceneDirty = false;
	State.ClearSelection();
}

// -----------------------------------------------------------------------
// Gizmo helpers — build model matrix from entity fields, write back after manipulation
// -----------------------------------------------------------------------

/// Find a float field pointer by debug name in an archetype's field array table.
static SimFloat* FindFieldFloat(Archetype* arch, void** fieldArrayTable, const char* name, uint32_t localIndex)
{
	const auto& cfr = ReflectionRegistry::Get();

	for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
	{
		if (fdesc.valueType != FieldValueType::Float32 && fdesc.valueType != FieldValueType::Fixed32) continue;
		if (!fieldArrayTable[fdesc.fieldSlotIndex]) continue;

		const auto* fields    = cfr.GetFields(fdesc.componentID);
		const char* fieldName = (fields && fdesc.componentSlotIndex < fields->size())
									? (*fields)[fdesc.componentSlotIndex].Name
									: nullptr;

		if (fieldName && std::strcmp(fieldName, name) == 0)
		{
			return static_cast<SimFloat*>(fieldArrayTable[fdesc.fieldSlotIndex]) + localIndex;
		}
	}
	return nullptr;
}


void EditorContext::DrawGizmo()
{
	// Only draw gizmo when an entity is selected
	if (State.Selection != EditorState::SelectionType::Entity) return;
	if (!State.SelectedArchetype || !State.SelectedChunk) return;
	if (!State.RegistryPtr) return;

	Archetype* arch = State.SelectedArchetype;

	// Get view and projection matrices from the frame header
	ComponentCacheBase* tc   = State.RegistryPtr->GetTemporalCache();
	TemporalFrameHeader* hdr = tc->GetFrameHeader();
	if (!hdr) return;

	// Build field array table for the selected entity's chunk
	uint32_t temporalFrame = tc->GetActiveWriteFrame();
	uint32_t volatileFrame = State.RegistryPtr->GetVolatileCache()->GetActiveWriteFrame();

	void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
	arch->BuildFieldArrayTable(State.SelectedChunk, fieldArrayTable, temporalFrame, volatileFrame);

	uint32_t li = State.SelectedLocalIndex;

	// Read transform fields
	SimFloat* pPosX = FindFieldFloat(arch, fieldArrayTable, "PosX", li);
	SimFloat* pPosY = FindFieldFloat(arch, fieldArrayTable, "PosY", li);
	SimFloat* pPosZ = FindFieldFloat(arch, fieldArrayTable, "PosZ", li);
	if (!pPosX || !pPosY || !pPosZ) return; // No position — can't place gizmo

	SimFloat* pRotQx = FindFieldFloat(arch, fieldArrayTable, "RotQx", li);
	SimFloat* pRotQy = FindFieldFloat(arch, fieldArrayTable, "RotQy", li);
	SimFloat* pRotQz = FindFieldFloat(arch, fieldArrayTable, "RotQz", li);
	SimFloat* pRotQw = FindFieldFloat(arch, fieldArrayTable, "RotQw", li);

	SimFloat* pScaleX = FindFieldFloat(arch, fieldArrayTable, "ScaleX", li);
	SimFloat* pScaleY = FindFieldFloat(arch, fieldArrayTable, "ScaleY", li);
	SimFloat* pScaleZ = FindFieldFloat(arch, fieldArrayTable, "ScaleZ", li);

	// Read raw floats from the fields
	float px = (*pPosX).ToFloat();
	float py = (*pPosY).ToFloat();
	float pz = (*pPosZ).ToFloat();
	float qx = pRotQx ? (*pRotQx).ToFloat() : 0.0f;
	float qy = pRotQy ? (*pRotQy).ToFloat() : 0.0f;
	float qz = pRotQz ? (*pRotQz).ToFloat() : 0.0f;
	float qw = pRotQw ? (*pRotQw).ToFloat() : 1.0f;
	float sx = pScaleX ? (*pScaleX).ToFloat() : 1.0f;
	float sy = pScaleY ? (*pScaleY).ToFloat() : 1.0f;
	float sz = pScaleZ ? (*pScaleZ).ToFloat() : 1.0f;

	// Build column-major transformation matrix from quaternion + scale + translation
	float x2 = qx + qx, y2 = qy + qy, z2 = qz + qz;
	float xx = qx * x2, xy = qx * y2, xz = qx * z2;
	float yy = qy * y2, yz = qy * z2, zz = qz * z2;
	float wx = qw * x2, wy = qw * y2, wz = qw * z2;

	float modelMatrix[16];
	modelMatrix[0] = (1.0f - (yy + zz)) * sx;
	modelMatrix[1] = (xy + wz) * sx;
	modelMatrix[2] = (xz - wy) * sx;
	modelMatrix[3] = 0.0f;

	modelMatrix[4] = (xy - wz) * sy;
	modelMatrix[5] = (1.0f - (xx + zz)) * sy;
	modelMatrix[6] = (yz + wx) * sy;
	modelMatrix[7] = 0.0f;

	modelMatrix[8]  = (xz + wy) * sz;
	modelMatrix[9]  = (yz - wx) * sz;
	modelMatrix[10] = (1.0f - (xx + yy)) * sz;
	modelMatrix[11] = 0.0f;

	modelMatrix[12] = px;
	modelMatrix[13] = py;
	modelMatrix[14] = pz;
	modelMatrix[15] = 1.0f;

	// Set ImGuizmo to cover the editor viewport panel
	ImGuizmo::SetDrawlist(); // bind to the current window's draw list for correct hit-testing
	ImGuizmo::SetRect(ViewportPanelPos.x, ViewportPanelPos.y, ViewportPanelSize.x, ViewportPanelSize.y);
	ImGuizmo::SetOrthographic(false);

	// Rebuild view and projection matrices from the frame header's quat+position+FoV.
	// ImGuizmo expects column-major, OpenGL-style (no Vulkan Y-flip in projection).
	const Quatf camRot = hdr->CameraRotation.ToFloat();
	const float crx = camRot.x, cry = camRot.y, crz = camRot.z, crw = camRot.w;

	// quatRotate(q, v): right = q*(1,0,0), up = q*(0,1,0), fwd = q*(0,0,-1)
	auto qr = [&](float vx, float vy, float vz, float& ox, float& oy, float& oz)
	{
		float tx = 2.0f * (cry * vz - crz * vy);
		float ty = 2.0f * (crz * vx - crx * vz);
		float tz = 2.0f * (crx * vy - cry * vx);
		ox = vx + crw * tx + (cry * tz - crz * ty);
		oy = vy + crw * ty + (crz * tx - crx * tz);
		oz = vz + crw * tz + (crx * ty - cry * tx);
	};

	float rx, ry, rz, ux, uy, uz, fx, fy, fz;
	qr( 1,  0,  0, rx, ry, rz);  // right
	qr( 0,  1,  0, ux, uy, uz);  // up
	qr( 0,  0, -1, fx, fy, fz);  // forward (-Z)

	const float cpx = hdr->CameraPosition.x.ToFloat();
	const float cpy = hdr->CameraPosition.y.ToFloat();
	const float cpz = hdr->CameraPosition.z.ToFloat();

	// Column-major view matrix
	Matrix4f viewFixup;
	viewFixup[0]  = rx; viewFixup[1]  = ux; viewFixup[2]  = -fx; viewFixup[3]  = 0;
	viewFixup[4]  = ry; viewFixup[5]  = uy; viewFixup[6]  = -fy; viewFixup[7]  = 0;
	viewFixup[8]  = rz; viewFixup[9]  = uz; viewFixup[10] = -fz; viewFixup[11] = 0;
	viewFixup[12] = -(rx*cpx + ry*cpy + rz*cpz);
	viewFixup[13] = -(ux*cpx + uy*cpy + uz*cpz);
	viewFixup[14] =  (fx*cpx + fy*cpy + fz*cpz);
	viewFixup[15] = 1;

	// Column-major projection (OpenGL-style, no Y-flip — ImGuizmo adds its own)
	const float aspect = (ViewportPanelSize.y > 0.f) ? ViewportPanelSize.x / ViewportPanelSize.y : 1.0f;
	const float fovRad  = hdr->CameraFoV.ToFloat() * 3.14159265f / 180.0f;
	const float F       = 1.0f / std::tan(fovRad * 0.5f);
	const float zNear   = 0.1f, zFar = 5000.0f;
	const float dz      = zNear - zFar;

	Matrix4f projFixup;
	for (int i = 0; i < 16; ++i) projFixup[i] = 0.0f;
	projFixup[0]  = F / aspect;
	projFixup[5]  = F;               // Y-up (no Vulkan flip)
	projFixup[10] = zFar / dz;
	projFixup[11] = -1.0f;
	projFixup[14] = (zFar * zNear) / dz;

	// Map our enum to ImGuizmo operation
	ImGuizmo::OPERATION op;
	switch (State.CurrentGizmoOp)
	{
		case EditorState::GizmoOp::Translate: op = ImGuizmo::TRANSLATE;
			break;
		case EditorState::GizmoOp::Rotate: op = ImGuizmo::ROTATE;
			break;
		case EditorState::GizmoOp::Scale: op = ImGuizmo::SCALE;
			break;
	}

	ImGuizmo::MODE mode = State.bGizmoWorldMode ? ImGuizmo::WORLD : ImGuizmo::LOCAL;

	// Snap values
	float snapValues[3]  = {};
	const float* snapPtr = nullptr;
	if (State.bGizmoSnap)
	{
		float snapVal = 0.0f;
		switch (State.CurrentGizmoOp)
		{
			case EditorState::GizmoOp::Translate: snapVal = State.GizmoSnapTranslate;
				break;
			case EditorState::GizmoOp::Rotate: snapVal = State.GizmoSnapRotate;
				break;
			case EditorState::GizmoOp::Scale: snapVal = State.GizmoSnapScale;
				break;
		}
		snapValues[0] = snapValues[1] = snapValues[2] = snapVal;
		snapPtr       = snapValues;
	}

	// Manipulate — modifies modelMatrix in-place if the user drags
	bool manipulated = ImGuizmo::Manipulate(
		viewFixup.m, projFixup.m,
		op, mode, modelMatrix, nullptr, snapPtr);

	if (manipulated)
	{
	    // Original: decompose and write values
		float translation[3], rotation[3], scale[3];
		ImGuizmo::DecomposeMatrixToComponents(modelMatrix, translation, rotation, scale);

		// --- Undo: capture before state ---
		auto cmd = std::make_unique<EntityTransformCommand>(
			arch, State.SelectedChunk, State.SelectedLocalIndex, State.RegistryPtr);

		// Write new values (same as original)
		*pPosX = SimFloat(translation[0]);
		*pPosY = SimFloat(translation[1]);
		*pPosZ = SimFloat(translation[2]);
		if (pRotQx && pRotQy && pRotQz && pRotQw)
		{
			float rx = rotation[0] * (3.14159265358979f / 180.0f) * 0.5f;
			float ry = rotation[1] * (3.14159265358979f / 180.0f) * 0.5f;
			float rz = rotation[2] * (3.14159265358979f / 180.0f) * 0.5f;
			float cx = std::cos(rx), sx2 = std::sin(rx);
			float cy = std::cos(ry), sy2 = std::sin(ry);
			float cz = std::cos(rz), sz2 = std::sin(rz);
			*pRotQw  = SimFloat(cx * cy * cz + sx2 * sy2 * sz2);
			*pRotQx  = SimFloat(sx2 * cy * cz - cx * sy2 * sz2);
			*pRotQy  = SimFloat(cx * sy2 * cz + sx2 * cy * sz2);
			*pRotQz  = SimFloat(cx * cy * sz2 - sx2 * sy2 * cz);
		}
		if (pScaleX) *pScaleX = SimFloat(scale[0]);
		if (pScaleY) *pScaleY = SimFloat(scale[1]);
		if (pScaleZ) *pScaleZ = SimFloat(scale[2]);

		// --- Original dirty marking (restored from pre-undo code) ---
	    Archetype::FieldKey flagKey{
	        CacheSlotMeta<>::StaticTypeID(),
			ReflectionRegistry::Get().GetCacheSlotIndex(CacheSlotMeta<>::StaticTypeID()),
			0
		};
		auto* flagDesc = arch->ArchetypeFieldLayout.find(flagKey);
		if (flagDesc)
		{
			auto* base = static_cast<uint8_t*>(State.SelectedChunk->GetFieldPtr(flagDesc->fieldSlotIndex));
			if (base)
			{
				auto* cache                     = State.RegistryPtr->GetTemporalCache();
				auto* flags                     = reinterpret_cast<int32_t*>(cache->GetWriteFramePtr(base));
				flags[State.SelectedLocalIndex] |= static_cast<int32_t>(TemporalFlagBits::Dirty);
			}
		}

		// --- Undo: capture after state ---
		cmd->SetAfter(SerializeEntityFields(State.RegistryPtr, arch, State.SelectedChunk, State.SelectedLocalIndex));
		PushCommand(std::move(cmd));

		State.bSceneDirty = true;
	}
}

void EditorContext::ConsumePick()
{
#ifdef TNX_GPU_PICKING
	if (!EnginePtr || !EnginePtr->Render) return;

#ifndef TNX_GPU_PICKING_FAST
	// On-demand mode: request a pick when the user clicks inside the 3D viewport panel.
	// WantCaptureMouse is always true over ImGui::Image(), so we use ViewportPanelHovered instead.
	if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ViewportPanelHovered
		&& !ImGuizmo::IsOver())
	{
		ImVec2 mousePos = ImGui::GetMousePos();

		// Scale to physical pixels for DPI
		int logicalW = 0, physicalW = 0;
		SDL_GetWindowSize(EnginePtr->GetWindow(), &logicalW, nullptr);
		SDL_GetWindowSizeInPixels(EnginePtr->GetWindow(), &physicalW, nullptr);
		const float dpiScale = (logicalW > 0) ? static_cast<float>(physicalW) / static_cast<float>(logicalW) : 1.0f;

		// Convert from global window coords to viewport-panel-relative coords,
		// then DPI-scale to match the offscreen pick target resolution.
		int32_t pickX = static_cast<int32_t>((mousePos.x - ViewportPanelPos.x) * dpiScale);
		int32_t pickY = static_cast<int32_t>((mousePos.y - ViewportPanelPos.y) * dpiScale);

		EnginePtr->Render->RequestPick(pickX, pickY);
	}
#endif

	uint32_t cacheIdx = 0;
	if (!EnginePtr->Render->ConsumePickResult(cacheIdx)) return;

	// UINT32_MAX = no entity hit (background)
	if (cacheIdx == UINT32_MAX)
	{
		// Only clear on explicit click (not passive mouse movement in FAST mode).
		// In FAST mode the result updates every frame — only act on left-click.
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ViewportPanelHovered
			&& !ImGuizmo::IsOver())
		{
			State.ClearSelection();
		}
		return;
	}

	// Only select on left-click, not passive hover
	if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
	if (!ViewportPanelHovered) return;
	if (ImGuizmo::IsOver()) return;

	// Resolve cache index → entity record via O(1) registry lookup
	Registry* reg = State.RegistryPtr;
	if (!reg) return;

	EntityRecord record = reg->GetRecordByCache(static_cast<EntityCacheHandle>(cacheIdx));
	if (!record.IsValid()) return;

	State.ClearSelection();
	State.Selection          = EditorState::SelectionType::Entity;
	State.SelectedClassID    = record.Arch->ArchClassID;
	State.SelectedArchetype  = record.Arch;
	State.SelectedChunk      = record.TargetChunk;
	State.SelectedLocalIndex = static_cast<uint16_t>(record.LocalIndex);
	State.SelectedCacheIndex = cacheIdx;
#endif
}

void EditorContext::BuildFrame()
{
	BuildDockspace();

	// Consume GPU pick results and update selection
	ConsumePick();

	// Main editor scene viewport — always visible, dockable
	DrawEditorViewportPanel();

	// Draw all panels
	for (auto& panel : Panels)
	{
		panel->Tick(State);
	}

	// Editor hotkeys — all gated behind WantTextInput so they don't fire inside text fields
	const ImGuiIO& io = ImGui::GetIO();
	if (!io.WantTextInput)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_W)) State.CurrentGizmoOp = EditorState::GizmoOp::Translate;
		if (ImGui::IsKeyPressed(ImGuiKey_E)) State.CurrentGizmoOp = EditorState::GizmoOp::Rotate;
		if (ImGui::IsKeyPressed(ImGuiKey_R)) State.CurrentGizmoOp = EditorState::GizmoOp::Scale;
		if (ImGui::IsKeyPressed(ImGuiKey_Z) && io.KeyCtrl && !io.KeyShift) Undo();
		if (ImGui::IsKeyPressed(ImGuiKey_Y) && io.KeyCtrl) Redo();
	}

	// Modals / overlays
	DrawFileDialog();
	DrawImportDialog();
	DrawUnsavedWarning();
	DrawPrefabSaveDialog();

	// PIE viewport panels
	if (bPIEActive)
	{
		if (ServerViewport) DrawViewportPanel("Server", *ServerViewport);
		for (size_t i = 0; i < PIEClients.size(); ++i)
		{
			char title[32];
			snprintf(title, sizeof(title), "Client %zu", i + 1);
			DrawViewportPanel(title, *PIEClients[i].Viewport);
		}
	}

	// Debug windows
	if (bShowDemoWindow) ImGui::ShowDemoWindow(&bShowDemoWindow);
	if (bShowMetrics) ImGui::ShowMetricsWindow(&bShowMetrics);

	// Tell Sentinel whether the engine should own input.
	// Engine gets input when: right-click held in viewport, or Play is running.
	bool rightClickInViewport = ImGui::IsMouseDown(ImGuiMouseButton_Right) && ViewportPanelHovered;
	bool playing              = (LogicPtr && !LogicPtr->IsSimPaused() && bHasSnapshot) || bPIEActive;
	// Escape requests PIE stop — deferred to after the ImGui frame completes
	// so we don't free GPU resources (descriptor sets, images) mid-frame.
	if (bPIEActive && ImGui::IsKeyPressed(ImGuiKey_Escape)) bPIEStopRequested = true;

	// Shift+F1 toggles mouse between engine and editor during PIE/Play.
	// When released, editor gets mouse for panel interaction; re-press to return control.
	if (playing && ImGui::IsKeyPressed(ImGuiKey_F1) && io.KeyShift) bMouseReleasedDuringPlay = !bMouseReleasedDuringPlay;
	if (!playing) bMouseReleasedDuringPlay = false;
	bool engineGetsInput = (rightClickInViewport || playing) && !bMouseReleasedDuringPlay;
	EnginePtr->Render->SetEditorOwnsKeyboard(!engineGetsInput);
}

void EditorContext::PushCommand(std::unique_ptr<UndoCommand> cmd)
{
    // Try to merge with previous command
    if (UndoIndex > 0)
    {
        auto& last = UndoStack[UndoIndex - 1];
		if (last->MergeWith(*cmd)) return; // merged, discard new
	}

	// Truncate redo history
	UndoStack.resize(UndoIndex);

	// Push new command
	UndoStack.push_back(std::move(cmd));
	UndoIndex++;

	// Cap size
	if (UndoStack.size() > MaxUndo)
	{
		UndoStack.erase(UndoStack.begin());
		UndoIndex--;
	}
}

void EditorContext::Undo()
{
	if (!CanUndo()) return;
	UndoStack[UndoIndex - 1]->Undo();
	UndoIndex--;
	// Clear selection? Leave it for now.
}

void EditorContext::Redo()
{
	if (!CanRedo()) return;
	UndoStack[UndoIndex]->Execute();
	UndoIndex++;
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
	// SetNextWindowViewport removed in ImGui 1.90+ - windows auto-attach to correct viewport

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

	ImGui::Begin("EditorDockspace", nullptr, windowFlags);
	ImGui::PopStyleVar(3);

	ImGuiID dockspaceID = ImGui::GetID("EditorDockspaceID");
	ImGui::DockSpace(dockspaceID, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

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
	ImGui::DockBuilderDockWindow("Viewport", center);
	ImGui::DockBuilderDockWindow("Details", right);

	// Bottom: tabbed — Content Browser, Log, Engine Stats, Node Script, Component Generator
	ImGui::DockBuilderDockWindow("Content Browser", bottom);
	ImGui::DockBuilderDockWindow("Log", bottom);
	ImGui::DockBuilderDockWindow("Engine Stats", bottom);
	ImGui::DockBuilderDockWindow("Node Script", bottom);
	ImGui::DockBuilderDockWindow("Component Generator", bottom);

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
									  State.CurrentScenePath.c_str(),
									  State.SceneDefaultState.empty() ? nullptr : State.SceneDefaultState.c_str(),
									  State.SceneDefaultMode.empty() ? nullptr : State.SceneDefaultMode.c_str());
			State.bSceneDirty = false;
		}
		if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S"))
		{
			bShowFileDialog    = true;
			bFileDialogForSave = true;
			FileDialogPath     = State.CurrentScenePath;
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Save as Prefab...", nullptr, false,
		                    State.Selection == EditorState::SelectionType::Entity))
		{
			bShowPrefabSaveDialog = true;

			// Build default filename from entity's class name
			std::string defaultName = "NewPrefab";
			if (State.SelectedClassID != 0)
			{
				const auto& cfr       = ReflectionRegistry::Get();
				std::string debugName = "UnknownClass";
				for (const auto& entry : cfr.NameToClassID)
				{
					if (entry.second == State.SelectedClassID)
					{
						debugName = entry.first;
						break;
					}
				}
			}
			// Prepend content directory
			std::string contentDir = State.ConfigPtr ? std::string(State.ConfigPtr->ProjectDir) + "/content/" : "";
			FileDialogPath         = contentDir + defaultName + ".prefab";
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Import Mesh...", nullptr, false, MeshMgr != nullptr))
		{
			bShowImportDialog = true;
			ImportDialogPath.clear();
		}
		ImGui::Separator();
		ImGui::MenuItem("Exit", nullptr, false, false);
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Edit"))
	{
		if (ImGui::MenuItem("Undo", "Ctrl+Z", false, CanUndo())) Undo();
		if (ImGui::MenuItem("Redo", "Ctrl+Y", false, CanRedo())) Redo();
		ImGui::Separator();

		bool isTranslate = State.CurrentGizmoOp == EditorState::GizmoOp::Translate;
		bool isRotate    = State.CurrentGizmoOp == EditorState::GizmoOp::Rotate;
		bool isScale     = State.CurrentGizmoOp == EditorState::GizmoOp::Scale;

		if (ImGui::MenuItem("Translate", "W", isTranslate)) State.CurrentGizmoOp = EditorState::GizmoOp::Translate;
		if (ImGui::MenuItem("Rotate", "E", isRotate)) State.CurrentGizmoOp = EditorState::GizmoOp::Rotate;
		if (ImGui::MenuItem("Scale", "R", isScale)) State.CurrentGizmoOp = EditorState::GizmoOp::Scale;

		ImGui::Separator();
		ImGui::MenuItem("World Space", nullptr, &State.bGizmoWorldMode);
		ImGui::MenuItem("Snap", nullptr, &State.bGizmoSnap);

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
		bool simPaused = !LogicPtr || LogicPtr->IsSimPaused();

		if (ImGui::MenuItem("Play (Local)", nullptr, false, simPaused && !bPIEActive))
		{
			if (LogicPtr)
			{
				if (!bHasSnapshot) SnapshotScene();
				LogicPtr->SetSimPaused(false);

				// Route input to editor world and let game spawn a player
				EnginePtr->InputTargetWorld = EnginePtr->GetDefaultWorld();
				if (EnginePtr->OnPlayStarted.IsBound())
				{
					EnginePtr->OnPlayStarted(EnginePtr->GetDefaultWorld());
				}
			}
		}
		if (ImGui::MenuItem("Pause", nullptr, false, !simPaused && !bPIEActive))
		{
			if (LogicPtr) LogicPtr->SetSimPaused(true);
		}
		if (ImGui::MenuItem("Stop (Local)", nullptr, false, bHasSnapshot && !bPIEActive))
		{
			// Let game destroy its constructs before restoring snapshot
			if (EnginePtr->OnPlayStopped.IsBound())
			{
				EnginePtr->OnPlayStopped();
			}
			EnginePtr->InputTargetWorld = nullptr;

			if (LogicPtr) LogicPtr->SetSimPaused(true);
			RestoreSnapshot();
		}

		ImGui::Separator();

		ImGui::SetNextItemWidth(80);
		ImGui::InputInt("Clients", &PIEClientCount, 1, 1);
		if (PIEClientCount < 1) PIEClientCount = 1;
		if (PIEClientCount > 4) PIEClientCount = 4;

		// Default State/Mode dropdowns (populated from ReflectionRegistry)
		{
			auto& rr = ReflectionRegistry::Get();

			// FlowState combo
			ImGui::SetNextItemWidth(160);
			const char* statePreview = State.SceneDefaultState.empty() ? "(none)" : State.SceneDefaultState.c_str();
			if (ImGui::BeginCombo("Default State", statePreview))
			{
				if (ImGui::Selectable("(none)", State.SceneDefaultState.empty()))
				{
					State.SceneDefaultState.clear();
					State.bSceneDirty = true;
				}
				for (const auto& entry : rr.RegisteredStates)
				{
					bool selected = (State.SceneDefaultState == entry.Name);
					if (ImGui::Selectable(entry.Name, selected))
					{
						State.SceneDefaultState = entry.Name;
						State.bSceneDirty       = true;
					}
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}

			// GameMode combo
			ImGui::SetNextItemWidth(160);
			const char* modePreview = State.SceneDefaultMode.empty() ? "(none)" : State.SceneDefaultMode.c_str();
			if (ImGui::BeginCombo("Default Mode", modePreview))
			{
				if (ImGui::Selectable("(none)", State.SceneDefaultMode.empty()))
				{
					State.SceneDefaultMode.clear();
					State.bSceneDirty = true;
				}
				for (const auto& entry : rr.RegisteredModes)
				{
					bool selected = (State.SceneDefaultMode == entry.Name);
					if (ImGui::Selectable(entry.Name, selected))
					{
						State.SceneDefaultMode = entry.Name;
						State.bSceneDirty      = true;
					}
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}

		ImGui::Separator();

		if (ImGui::MenuItem("Play (Server + Client)", nullptr, false, !bPIEActive))
		{
			bServerVisible = true;
			StartPIE();
		}
		if (ImGui::MenuItem("Play (Headless Server + Client)", nullptr, false, !bPIEActive))
		{
			bServerVisible = false;
			StartPIE();
		}
		if (ImGui::MenuItem("Stop PIE", nullptr, false, bPIEActive))
		{
			StopPIE();
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

				EntityBuilder::SaveToFile(State.RegistryPtr, name.c_str(), FileDialogPath.c_str(),
										  State.SceneDefaultState.empty() ? nullptr : State.SceneDefaultState.c_str(),
										  State.SceneDefaultMode.empty() ? nullptr : State.SceneDefaultMode.c_str());
				State.CurrentScenePath = FileDialogPath;
				State.CurrentSceneName = name;
				State.bSceneDirty      = false;
			}
			else
			{
				LoadScene(FileDialogPath);
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

void EditorContext::DrawPrefabSaveDialog()
{
	if (!bShowPrefabSaveDialog) return;

	ImGui::OpenPopup("Save Prefab As");
	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(500, 120), ImGuiCond_Appearing);

	if (ImGui::BeginPopupModal("Save Prefab As", &bShowPrefabSaveDialog, ImGuiWindowFlags_AlwaysAutoResize))
	{
		char pathBuf[512];
		snprintf(pathBuf, sizeof(pathBuf), "%s", FileDialogPath.c_str());

		// Show the path relative to content directory for clarity
		std::string relativePath;
		if (State.ConfigPtr)
		{
			std::string contentDir = std::string(State.ConfigPtr->ProjectDir) + "/content/";
			if (FileDialogPath.find(contentDir) == 0) relativePath = FileDialogPath.substr(contentDir.length());
			else relativePath                                      = FileDialogPath;
		}
		ImGui::Text("Save prefab to:  %s", relativePath.c_str());

		ImGui::SetNextItemWidth(-1);
		if (ImGui::InputText("##prefabpath", pathBuf, sizeof(pathBuf))) FileDialogPath = pathBuf;

		ImGui::Separator();
		if (ImGui::Button("Save", ImVec2(120, 0)))
		{
			// Prepare full path with content directory and .prefab extension
			std::string finalPath = FileDialogPath;

			// Prepend content directory if not already present
			if (State.ConfigPtr)
			{
				std::string contentDir = std::string(State.ConfigPtr->ProjectDir) + "/content/";
				if (finalPath.find(contentDir) != 0) finalPath = contentDir + finalPath;
			}

			// Ensure .prefab extension
			if (finalPath.size() < 7 || finalPath.substr(finalPath.size() - 7) != ".prefab") finalPath += ".prefab";

			if (State.Selection == EditorState::SelectionType::Entity)
			{
				Registry* reg = State.RegistryPtr;

				// Serialize entity fields
				JsonValue components = SerializeEntityFields(reg,
															 State.SelectedArchetype, State.SelectedChunk, State.SelectedLocalIndex);

				// Wrap in prefab JSON (type + components)
				JsonValue prefabJson = JsonValue::Object();
				// Look up class name from ClassID
				std::string typeName   = "Unknown";
				const auto& archetypes = reg->GetArchetypes();
				for (const auto& entry : archetypes)
				{
					if (entry.first.ID == State.SelectedClassID)
					{
						typeName = entry.second->DebugName;
						break;
					}
				}
				prefabJson["type"]       = JsonValue::String(typeName);
				prefabJson["components"] = components;

				std::string jsonStr = JsonWrite(prefabJson, true);
				std::ofstream file(finalPath);
				if (file.is_open())
				{
					file << jsonStr;
					file.close();
					LOG_ENG_INFO_F("[Editor] Saved prefab to %s", finalPath.c_str());
				}
				else
					LOG_ENG_ERROR("[Editor] Failed to write prefab file");
			}

			bShowPrefabSaveDialog = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0)))
		{
			bShowPrefabSaveDialog = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

void EditorContext::DrawImportDialog()
{
	if (!bShowImportDialog) return;

	ImGui::OpenPopup("Import Mesh");

	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(500, 120), ImGuiCond_Appearing);

	if (ImGui::BeginPopupModal("Import Mesh", &bShowImportDialog, ImGuiWindowFlags_AlwaysAutoResize))
	{
		char pathBuf[512]{};
		snprintf(pathBuf, sizeof(pathBuf), "%s", ImportDialogPath.c_str());

		ImGui::Text("Path to .gltf or .glb file:");
		ImGui::SetNextItemWidth(-1);
		if (ImGui::InputText("##importpath", pathBuf, sizeof(pathBuf))) ImportDialogPath = pathBuf;

		ImGui::Separator();

		if (ImGui::Button("Import", ImVec2(120, 0)))
		{
			uint32_t slot = ImportMeshAsset(ImportDialogPath);
			if (slot != UINT32_MAX)
				LOG_ENG_INFO_F("[Editor] Imported mesh → slot %u", slot);
			else
				LOG_ENG_ERROR_F("[Editor] Failed to import: %s", ImportDialogPath.c_str());

			bShowImportDialog = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0)))
		{
			bShowImportDialog = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}

uint32_t EditorContext::ImportMeshAsset(const std::string& gltfPath)
{
	if (!MeshMgr || !State.ConfigPtr) return UINT32_MAX;

	// Derive output .tnxmesh path in content/
	std::filesystem::path src(gltfPath);
	std::string stem    = src.stem().string();
	std::string outPath = std::string(State.ConfigPtr->ProjectDir)
		+ "/content/" + stem + ".tnxmesh";

	// Import glTF → .tnxmesh
	if (!ImportGLTF(gltfPath, outPath))
	{
		LOG_ENG_ERROR_F("[Editor] ImportGLTF failed: %s", gltfPath.c_str());
		return UINT32_MAX;
	}

	LOG_ENG_INFO_F("[Editor] Wrote %s", outPath.c_str());

	// Reconcile AssetDatabase to pick up the new file
	AssetDB.Reconcile();

	// Load the .tnxmesh into MeshManager
	MeshAsset asset;
	if (!LoadMeshAsset(asset, outPath))
	{
		LOG_ENG_ERROR_F("[Editor] LoadMeshAsset failed: %s", outPath.c_str());
		return UINT32_MAX;
	}

	// Look up AssetID from AssetDatabase after reconcile
	std::string relPath = stem + ".tnxmesh";
	const auto* dbEntry = AssetDB.FindByPath(relPath);
	AssetID meshID      = dbEntry ? dbEntry->ID : AssetID{};

	uint32_t slot = MeshMgr->LoadMesh(asset, stem, meshID);
	if (slot != UINT32_MAX)
	LOG_ENG_INFO_F("[Editor] Registered mesh '%s' at slot %u (AssetID: %lld)",
				   stem.c_str(), slot, static_cast<long long>(meshID.GetUUID() >> 8));

	return slot;
}

void EditorContext::LoadAllMeshAssets()
{
	if (!MeshMgr || !State.ConfigPtr) return;

	const auto& entries     = AssetDB.GetEntries();
	std::string contentBase = std::string(State.ConfigPtr->ProjectDir) + "/content/";

	for (const auto& entry : entries)
	{
		if (entry.Type != AssetType::StaticMesh) continue;

		// Only load .tnxmesh files (skip raw .gltf/.glb/.obj/.fbx source files)
		if (entry.Path.size() < 8 || entry.Path.substr(entry.Path.size() - 8) != ".tnxmesh") continue;

		std::string fullPath = contentBase + entry.Path;
		MeshAsset asset;
		if (!LoadMeshAsset(asset, fullPath))
		{
			LOG_ENG_WARN_F("[Editor] Failed to load mesh asset: %s", entry.Path.c_str());
			continue;
		}

		uint32_t slot = MeshMgr->LoadMesh(asset, entry.Name, entry.ID);
		if (slot != UINT32_MAX)
			LOG_ENG_INFO_F("[Editor] Loaded mesh '%s' → slot %u", entry.Path.c_str(), slot);
	}
}

void EditorContext::HandleDroppedFile(const std::string& path)
{
	// Check if it's a mesh file we can import
	std::filesystem::path p(path);
	std::string ext = p.extension().string();

	// Convert extension to lowercase for comparison
	for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

	if (ext == ".gltf" || ext == ".glb")
	{
		uint32_t slot = ImportMeshAsset(path);
		if (slot != UINT32_MAX)
			LOG_ENG_INFO_F("[Editor] Drag-and-drop imported mesh → slot %u", slot);
		else
			LOG_ENG_ERROR_F("[Editor] Failed to import dropped file: %s", path.c_str());
	}
	else if (ext == ".tnxmesh")
	{
		// Already in engine format — copy to content/ and load
		if (MeshMgr && State.ConfigPtr)
		{
			std::string destPath = std::string(State.ConfigPtr->ProjectDir)
				+ "/content/" + p.filename().string();
			if (path != destPath) std::filesystem::copy_file(p, destPath, std::filesystem::copy_options::overwrite_existing);

			AssetDB.Reconcile();

			MeshAsset asset;
			if (LoadMeshAsset(asset, destPath))
			{
				std::string relDropPath = p.filename().string();
				const auto* dropEntry   = AssetDB.FindByPath(relDropPath);
				AssetID dropID          = dropEntry ? dropEntry->ID : AssetID{};
				std::string dropName    = dropEntry ? dropEntry->Name : p.stem().string();

				uint32_t slot = MeshMgr->LoadMesh(asset, dropName, dropID);
				if (slot != UINT32_MAX)
					LOG_ENG_INFO_F("[Editor] Loaded dropped mesh '%s' → slot %u", dropName.c_str(), slot);
			}
		}
	}
	else if (ext == ".prefab")
	{
		SpawnPrefab(path);
	}
#ifndef TNX_HEADLESS
	else if (ext == ".wav" || ext == ".ogg")
	{
		// Convert source → .tnxaudio in content/. The source file is not copied.
		if (AudioMgr && State.ConfigPtr)
		{
			std::string stem    = p.stem().string();
			std::string outPath = std::string(State.ConfigPtr->ProjectDir)
				+ "/content/" + stem + ".tnxaudio";

			if (!ExportTnxAudio(path.c_str(), outPath.c_str()))
			{
				LOG_ENG_ERROR_F("[Editor] ExportTnxAudio failed for: %s", path.c_str());
			}
			else
			{
				AssetDB.Reconcile();

				const auto* dbEntry = AssetDB.FindByPath(stem + ".tnxaudio");
				AssetID audioID     = dbEntry ? dbEntry->ID : AssetID{};
				std::string name    = dbEntry ? dbEntry->Name : stem;

				uint32_t slot = AudioMgr->LoadSound(outPath.c_str(), name, audioID);
				if (slot != UINT32_MAX)
					LOG_ENG_INFO_F("[Editor] Imported audio '%s' → slot %u", name.c_str(), slot);
				else
					LOG_ENG_ERROR_F("[Editor] Failed to register imported audio: %s", outPath.c_str());
			}
		}
	}
	else if (ext == ".tnxaudio")
	{
		// Already engine format — copy to content/ and register.
		if (AudioMgr && State.ConfigPtr)
		{
			std::string destPath = std::string(State.ConfigPtr->ProjectDir)
				+ "/content/" + p.filename().string();
			if (path != destPath) std::filesystem::copy_file(p, destPath, std::filesystem::copy_options::overwrite_existing);

			AssetDB.Reconcile();

			std::string stem    = p.stem().string();
			const auto* dbEntry = AssetDB.FindByPath(p.filename().string());
			AssetID audioID     = dbEntry ? dbEntry->ID : AssetID{};
			std::string name    = dbEntry ? dbEntry->Name : stem;

			uint32_t slot = AudioMgr->LoadSound(destPath.c_str(), name, audioID);
			if (slot != UINT32_MAX)
				LOG_ENG_INFO_F("[Editor] Loaded dropped .tnxaudio '%s' → slot %u", name.c_str(), slot);
			else
				LOG_ENG_ERROR_F("[Editor] Failed to load dropped .tnxaudio: %s", path.c_str());
		}
	}
#endif
	else
	{
		LOG_ENG_WARN_F("[Editor] Unsupported drop file type: %s", ext.c_str());
	}
}

void EditorContext::SpawnPrefab(const std::string& prefabPath)
{
	Registry* prefabReg    = EnginePtr->GetDefaultWorld() ? EnginePtr->GetDefaultWorld()->GetRegistry() : nullptr;
	const char* prefabCStr = prefabPath.c_str();
	EnginePtr->Spawn([prefabReg, prefabCStr](uint32_t)
	{
		size_t count = EntityBuilder::SpawnFromFile(prefabReg, prefabCStr);
		if (count > 0)
			LOG_ENG_INFO_F("[Editor] Spawned %zu entities from prefab: %s", count, prefabCStr);
		else
			LOG_ENG_ERROR_F("[Editor] Failed to spawn prefab: %s", prefabCStr);
	});

	State.bSceneDirty = true;
}

void EditorContext::DeleteSelectedEntity()
{
    if (State.Selection != EditorState::SelectionType::Entity) return;

    // Capture undo data before deletion
    Archetype* arch     = State.SelectedArchetype;
    Chunk* chunk        = State.SelectedChunk;
	uint16_t localIndex = State.SelectedLocalIndex;
	//uint32_t cacheIndex    = State.SelectedCacheIndex;
	ClassID classID = State.SelectedClassID;
	Registry* reg   = State.RegistryPtr;

	// Serialize entity state while it still exists
	JsonValue beforeState = SerializeEntityFields(reg, arch, chunk, localIndex);

	// Perform deletion as before
	State.ClearSelection();

	Registry* deleteReg = EnginePtr->GetDefaultWorld() ? EnginePtr->GetDefaultWorld()->GetRegistry() : nullptr;
	EnginePtr->Spawn([deleteReg, chunk, localIndex](uint32_t)
	{
		EntityCacheHandle cacheIdx = chunk->Header.CacheIndexStart + localIndex;
		GlobalEntityHandle gHandle = deleteReg->FindEntityByLocation(cacheIdx);
		if (gHandle.GetIndex() == 0)
		{
			LOG_ENG_WARN("[Editor] Could not find entity to delete");
			return;
		}
		deleteReg->DestroyByGlobalHandle(gHandle);
		LOG_ENG_INFO_F("[Editor] Deleted entity (cache index %u)", cacheIdx);
	});

	// Create and push delete command (inlined class for simplicity)
	class UndoableDeleteCommand : public UndoCommand
	{
	public:
		UndoableDeleteCommand(TrinyxEngine* engine, Registry* reg, ClassID classID, JsonValue savedState)
			: m_Engine(engine)
			, m_Reg(reg), m_ClassID(classID), m_SavedState(std::move(savedState)) {}

		void Execute() override
		{
			if (m_RestoredCacheIdx == UINT32_MAX) return;
			uint32_t cacheIdx  = m_RestoredCacheIdx;
			m_RestoredCacheIdx = UINT32_MAX;
			m_Engine->Spawn([reg = m_Reg, cacheIdx](uint32_t)
			{
				GlobalEntityHandle gh = reg->FindEntityByLocation(static_cast<EntityCacheHandle>(cacheIdx));
				if (gh.GetIndex() == 0)
				{
					LOG_ENG_WARN("[Editor] Redo delete: entity not found");
					return;
				}
				reg->DestroyByGlobalHandle(gh);
			});
		}

		void Undo() override
		{
			m_Engine->Spawn([this](uint32_t)
			{
				EntityHandle handle = m_Reg->CreateByClassID(m_ClassID);
				EntityRecord record = m_Reg->GetRecord(handle);
				if (record.IsValid())
				{
					DeserializeEntityFields(m_Reg, record.Arch, record.TargetChunk, record.LocalIndex, m_SavedState);
					MarkEntityDirty(m_Reg, record.Arch, record.TargetChunk, record.LocalIndex);
					m_RestoredCacheIdx = record.TargetChunk->Header.CacheIndexStart + record.LocalIndex;
				}
			});
		}

	private:
		TrinyxEngine* m_Engine;
		Registry* m_Reg;
		ClassID m_ClassID;
		JsonValue m_SavedState;
		uint32_t m_RestoredCacheIdx = UINT32_MAX;
	};

	PushCommand(std::make_unique<UndoableDeleteCommand>(EnginePtr, reg, classID, std::move(beforeState)));
	State.bSceneDirty = true;
}

void EditorContext::SnapshotScene()
{
	PlaySnapshot.clear();

	Registry* reg = State.RegistryPtr;
	if (!reg) return;

	uint32_t temporalFrame = reg->GetTemporalCache()->GetActiveWriteFrame();
	uint32_t volatileFrame = reg->GetVolatileCache()->GetActiveWriteFrame();

	for (auto& [key, arch] : reg->GetArchetypes())
	{
		size_t fieldCount = arch->GetFieldArrayCount();
		if (fieldCount == 0) continue;

		ArchetypeSnapshot archSnap;
		archSnap.ArchClassID      = arch->ArchClassID;
		archSnap.TotalEntityCount = arch->TotalEntityCount;

		for (size_t ci = 0; ci < arch->Chunks.size(); ++ci)
		{
			Chunk* chunk         = arch->Chunks[ci];
			uint32_t entityCount = arch->GetAllocatedChunkCount(ci);
			if (entityCount == 0) continue;

			void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
			arch->BuildFieldArrayTable(chunk, fieldArrayTable, temporalFrame, volatileFrame);

			// Calculate total bytes needed for all fields in this chunk
			size_t totalBytes = 0;
			for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
			{
				if (fieldArrayTable[fdesc.fieldSlotIndex]) totalBytes += fdesc.fieldSize * entityCount;
			}

			ArchetypeSnapshot::ChunkData chunkData;
			chunkData.Chunk       = chunk;
			chunkData.EntityCount = entityCount;
			chunkData.FieldData.resize(totalBytes);

			// Copy field data into snapshot
			size_t offset = 0;
			for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
			{
				if (!fieldArrayTable[fdesc.fieldSlotIndex]) continue;

				size_t bytes = fdesc.fieldSize * entityCount;
				std::memcpy(chunkData.FieldData.data() + offset, fieldArrayTable[fdesc.fieldSlotIndex], bytes);
				offset += bytes;
			}

			archSnap.Chunks.push_back(std::move(chunkData));
		}

		if (!archSnap.Chunks.empty()) PlaySnapshot.push_back(std::move(archSnap));
	}

	bHasSnapshot = true;
	LOG_ENG_INFO("[Editor] Scene snapshot taken for Play session");
}

void EditorContext::RestoreSnapshot()
{
	if (!bHasSnapshot) return;

	Registry* snapReg = EnginePtr->GetDefaultWorld() ? EnginePtr->GetDefaultWorld()->GetRegistry() : nullptr;
	EnginePtr->Spawn([this, snapReg](uint32_t)
	{
		Registry* reg = snapReg;
		if (!reg) return;

		// Reset all Jolt bodies — Play may have created bodies that don't exist in the snapshot.
		// FlushPendingBodies will recreate them from the restored field data on the next physics tick.
		if (EnginePtr->GetDefaultWorld() && EnginePtr->GetDefaultWorld()->GetPhysics()) EnginePtr->GetDefaultWorld()->GetPhysics()->ResetAllBodies();

		uint32_t temporalFrame = reg->GetTemporalCache()->GetActiveWriteFrame();
		uint32_t volatileFrame = reg->GetVolatileCache()->GetActiveWriteFrame();

		for (auto& archSnap : PlaySnapshot)
		{
			// Find the archetype by ClassID
			Archetype* ownerArch = nullptr;
			for (auto& [key, arch] : reg->GetArchetypes())
			{
				if (arch->ArchClassID == archSnap.ArchClassID)
				{
					ownerArch = arch;
					break;
				}
			}

			if (!ownerArch) continue;

			// Restore per-chunk field data
			for (auto& chunkSnap : archSnap.Chunks)
			{
				Chunk* chunk = static_cast<Chunk*>(chunkSnap.Chunk);

				void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
				ownerArch->BuildFieldArrayTable(chunk, fieldArrayTable, temporalFrame, volatileFrame);

				// Restore field data — only for the entity count we snapshotted.
				// If the chunk now has more entities (spawned during Play), the snapshot
				// data covers only the original ones; extras get their Active flag cleared below.
				size_t offset = 0;
				for (const auto& [fkey, fdesc] : ownerArch->ArchetypeFieldLayout)
				{
					if (!fieldArrayTable[fdesc.fieldSlotIndex]) continue;

					size_t bytes = fdesc.fieldSize * chunkSnap.EntityCount;
					std::memcpy(fieldArrayTable[fdesc.fieldSlotIndex], chunkSnap.FieldData.data() + offset, bytes);
					offset += bytes;
				}
			}

			// Handle entities created during Play: tombstone them by clearing Active flag.
			// The snapshot restores the original field data (including Active flags for original
			// entities). Entities beyond the snapshot count need to be deactivated.
			if (ownerArch->TotalEntityCount > archSnap.TotalEntityCount)
			{
				uint32_t extraCount = ownerArch->TotalEntityCount - archSnap.TotalEntityCount;
				LOG_ENG_INFO_F("[Editor] Tombstoning %u entities created during Play in archetype %u",
							   extraCount, archSnap.ArchClassID);

				// Look up the Flags field descriptor once
				Archetype::FieldKey flagKey{CacheSlotMeta<>::StaticTypeID(), ReflectionRegistry::Get().GetCacheSlotIndex(CacheSlotMeta<>::StaticTypeID()), 0};
				auto* flagDesc = ownerArch->ArchetypeFieldLayout.find(flagKey);

				uint32_t entityIdx = archSnap.TotalEntityCount;
				while (entityIdx < ownerArch->TotalEntityCount)
				{
					uint32_t chunkIdx = entityIdx / ownerArch->EntitiesPerChunk;
					uint32_t localIdx = entityIdx % ownerArch->EntitiesPerChunk;

					if (chunkIdx < ownerArch->Chunks.size() && flagDesc)
					{
						Chunk* chunk = ownerArch->Chunks[chunkIdx];

						void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
						ownerArch->BuildFieldArrayTable(chunk, fieldArrayTable, temporalFrame, volatileFrame);

						auto* flagsArr     = static_cast<int32_t*>(fieldArrayTable[flagDesc->fieldSlotIndex]);
						flagsArr[localIdx] = static_cast<int32_t>(TemporalFlagBits::Dirty);
					}

					entityIdx++;
				}
			}
			else if (ownerArch->TotalEntityCount < archSnap.TotalEntityCount)
			{
				// Entities were deleted during Play (swap-and-pop). Field data for surviving
				// entities has been restored, but the deleted ones can't be reconstructed without
				// full entity records. This will be properly solved by PIE world duplication.
				LOG_ENG_INFO_F("[Editor] Warning: %u entities were deleted during Play in archetype %u — "
							   "deleted entities cannot be restored (PIE world duplication needed)",
						   archSnap.TotalEntityCount - ownerArch->TotalEntityCount, archSnap.ArchClassID);
			}
		}
	});

	// Clear selection since entity indices may have changed
	State.ClearSelection();

	PlaySnapshot.clear();
	bHasSnapshot = false;
	LOG_ENG_INFO("[Editor] Scene restored from snapshot");
}

// -----------------------------------------------------------------------
// PIE — networked multi-world Play-In-Editor
// -----------------------------------------------------------------------

void EditorContext::StartPIE()
{
	if (bPIEActive) return;

	Registry* editorReg = EnginePtr->GetRegistry();

	// Serialize editor scene to JSON
	JsonValue sceneJson = EntityBuilder::SerializeScene(editorReg, "PIE");

	// Build server and client configs from the game config (no editor overrides)
	ServerConfig = *EnginePtr->GetGameConfig();

	// Create server flow (owns server world + constructs)
	ServerFlow = std::make_unique<PIEServerFlow>();
	ServerFlow->Initialize(EnginePtr, &ServerConfig, 960, 540);
	if (!ServerFlow->CreateWorld())
	{
		LOG_ENG_ERROR("[PIE] Failed to initialize server world");
		ServerFlow.reset();
		return;
	}
	WorldBase* AuthorityWorld = ServerFlow->GetWorld();

	// Load scene into server world via spawn handshake
	AuthorityWorld->SetJobsInitialized(true);

	// Allocate server viewport (if visible)
	EditorRenderer* renderer = EnginePtr->GetRenderer();
	if (bServerVisible)
	{
		ServerViewport              = std::make_unique<WorldViewport>();
		ServerViewport->TargetWorld = AuthorityWorld;
		renderer->AllocateViewportResources(ServerViewport.get(), 960, 540);
		renderer->AddViewport(ServerViewport.get());
	}

	// Create client worlds (each with its own FlowManager)
	PIEClients.reserve(PIEClientCount);
	for (int ci = 0; ci < PIEClientCount; ++ci)
	{
		PIEClient client;
		client.Config = *EnginePtr->GetGameConfig();
		client.Flow   = std::make_unique<PIEClientFlow>();
		client.Flow->Initialize(EnginePtr, &client.Config, 960, 540);
		if (!client.Flow->CreateWorld())
		{
			LOG_ENG_ERROR_F("[PIE] Failed to initialize client world %d", ci);
			// Clean up server + already-created clients
			for (auto& c : PIEClients)
			{
				renderer->RemoveViewport(c.Viewport.get());
				renderer->FreeViewportResources(c.Viewport.get());
			}
			if (ServerViewport)
			{
				renderer->RemoveViewport(ServerViewport.get());
				renderer->FreeViewportResources(ServerViewport.get());
				ServerViewport.reset();
			}
			PIEClients.clear();
			ServerFlow.reset();
			return;
		}
		WorldBase* clientWorld = client.Flow->GetWorld();
		clientWorld->SetJobsInitialized(true);

		client.Viewport              = std::make_unique<WorldViewport>();
		client.Viewport->TargetWorld = clientWorld;
		renderer->AllocateViewportResources(client.Viewport.get(), 960, 540);
		renderer->AddViewport(client.Viewport.get());

		PIEClients.push_back(std::move(client));
		// Re-point FlowManager at the stable Config now that the struct is in the vector.
		PIEClients.back().Flow->RewireConfig(&PIEClients.back().Config);
	}

	// Start all logic threads now — before networking — so the Logic Thread is
	// already spinning when the handshake pump runs. This matches real gameplay
	// where the world exists before any network layer touches it.
	ServerFlow->StartWorld();
	for (auto& c : PIEClients) c.Flow->StartWorld();

	// Set up loopback networking (server + client in same process)
	static constexpr uint16_t PIEPort = 27015;

	if (!EnginePtr->EnsureNetworking())
	{
		LOG_ENG_ERROR("[PIE] Failed to initialize networking — aborting");
		// Clean up viewports
		for (auto& c : PIEClients)
		{
			renderer->RemoveViewport(c.Viewport.get());
			renderer->FreeViewportResources(c.Viewport.get());
		}
		if (ServerViewport)
		{
			renderer->RemoveViewport(ServerViewport.get());
			renderer->FreeViewportResources(ServerViewport.get());
		}
		PIEClients.clear();
		ServerViewport.reset();
		ServerFlow.reset();
		return;
	}

	PIENetThread* net             = EnginePtr->GetNetThread();
	NetConnectionManager* connMgr = net->GetConnectionManager();

	// Server: listen on PIE loopback port
	if (!connMgr->Listen(PIEPort))
	{
		LOG_ENG_ERROR("[PIE] Failed to listen — aborting");
		for (auto& c : PIEClients)
		{
			renderer->RemoveViewport(c.Viewport.get());
			renderer->FreeViewportResources(c.Viewport.get());
		}
		if (ServerViewport)
		{
			renderer->RemoveViewport(ServerViewport.get());
			renderer->FreeViewportResources(ServerViewport.get());
		}
		PIEClients.clear();
		ServerViewport.reset();
		ServerFlow.reset();
		return;
	}

	// Wire the server world pointer before clients connect so that ConnectionHandshake
	// processing (EnsurePlayerInputSlot) finds a valid AuthorityWorld.
	net->SetAuthorityWorld(ServerFlow->GetWorld());

	// ReplicationSystem must exist before the pump loop — HandshakeRequest → GenerateNetID
	// → CreateInputLog → Replicator->OpenChannel fires during the pump, not after.
	Replicator = std::make_unique<ReplicationSystem>();
	Replicator->Initialize(ServerFlow->GetWorld());
	ServerFlow->GetWorld()->SetReplicationSystem(Replicator.get());
	net->SetReplicationSystem(Replicator.get());

	// Connect each client via loopback and discover server-side handles
	std::vector<uint32_t> knownHandles;
	for (const auto& ci : connMgr->GetConnections()) knownHandles.push_back(ci.Handle);

	for (size_t i = 0; i < PIEClients.size(); ++i)
	{
		uint32_t clientHandle = connMgr->Connect("127.0.0.1", PIEPort);
		if (clientHandle == 0)
		{
			LOG_ENG_ERROR_F("[PIE] Client %zu failed to connect — aborting", i);
			connMgr->StopListening();
			for (auto& c : PIEClients)
			{
				renderer->RemoveViewport(c.Viewport.get());
				renderer->FreeViewportResources(c.Viewport.get());
			}
			if (ServerViewport)
			{
				renderer->RemoveViewport(ServerViewport.get());
				renderer->FreeViewportResources(ServerViewport.get());
			}
			PIEClients.clear();
			ServerViewport.reset();
			ServerFlow.reset();
			return;
		}
		PIEClients[i].ClientHandle = clientHandle;
		knownHandles.push_back(clientHandle);

		// Register the client handler immediately — before the pump — so it
		// can receive the handshake reply and subsequent ClockSync/TravelNotify.
		// OwnerID is 0 at this point; PIENetThread routes by handle until promoted.
		net->AddClient(clientHandle, PIEClients[i].Flow->GetWorld());

		// Pump: run callbacks + poll + dispatch until the server-side connection appears
		// and GenerateNetID has fired (HandshakeRequest processed → OwnerID assigned).
		const HSteamNetConnection serverHandle = [&]() -> HSteamNetConnection
		{
			for (int j = 0; j < 50; ++j)
			{
				net->PumpMessages();
				SDL_Delay(1);
				for (const auto& ci : connMgr->GetConnections())
				{
					bool known = false;
					for (uint32_t h : knownHandles) { if (h == ci.Handle) { known = true; break; } }
					if (!known) return ci.Handle;
				}
			}
			return 0;
		}();

		if (serverHandle == 0)
		{
			LOG_ENG_WARN_F("[PIE] Could not identify server-side handle for client %zu", i);
		}
		else
		{
			knownHandles.push_back(serverHandle);
			PIEClients[i].ServerHandle = serverHandle;

			// Keep pumping until GenerateNetID fires and OwnerID is non-zero.
			uint8_t ownerID = 0;
			for (int j = 0; j < 100 && ownerID == 0; ++j)
			{
				net->PumpMessages();
				SDL_Delay(1);
				for (const auto& ci : connMgr->GetConnections())
				{
					if (ci.Handle == serverHandle) { ownerID = ci.OwnerID; break; }
				}
			}

			if (ownerID == 0)
				LOG_ENG_WARN_F("[PIE] OwnerID never assigned for client %zu server handle %u", i, serverHandle);

			// Promote client entry: wire world to the now-known OwnerID.
			PIEClients[i].Flow->GetWorld()->SetLocalOwnerID(ownerID);
			net->UpdateClientOwnerID(clientHandle, ownerID, PIEClients[i].Flow->GetWorld());
		}
	}

	net->GetAuthority().WireNetMode(ServerFlow->GetWorld());

	// PIENetThread is now driven by the Sentinel main loop — no Start() needed.

	if (EnginePtr->OnPIEStarted.IsBound())
		EnginePtr->OnPIEStarted(ServerFlow->GetWorld(), connMgr);

	LOG_ENG_INFO("[PIE] Worlds started");

	// Load scene default state/mode into flow managers (worlds + net already live)
	if (!State.SceneDefaultMode.empty())
	{
		ServerFlow->SetGameMode(State.SceneDefaultMode.c_str());
	}
	if (!State.SceneDefaultState.empty())
	{
		ServerFlow->LoadDefaultState(State.SceneDefaultState.c_str());
		for (auto& c : PIEClients) c.Flow->LoadDefaultState(State.SceneDefaultState.c_str());
	}

	// 9. Default input to first client world until a viewport panel gets focus
	if (!PIEClients.empty())
	{
		EnginePtr->InputTargetWorld = PIEClients[0].Flow->GetWorld();
	}

	// 10. Pause editor world's logic thread (save state so StopPIE restores correctly)
	bPrePIESimWasPaused = !LogicPtr || LogicPtr->IsSimPaused();
	if (LogicPtr) LogicPtr->SetSimPaused(true);

	bPIEActive = true;
	State.ClearSelection();
	LOG_ENG_INFO_F("[PIE] Started: 1 server%s + %zu client(s), port %u",
				   bServerVisible ? " (visible)" : " (headless)",
			   PIEClients.size(), PIEPort);
}

void EditorContext::StopPIE()
{
	if (!bPIEActive) return;

	// Clear input routing immediately
	EnginePtr->InputTargetWorld = nullptr;

	EditorRenderer* renderer = EnginePtr->GetRenderer();

	// 2. Tear down PIE networking
	PIENetThread* net = EnginePtr->GetNetThread();
	if (net)
	{
		// Detach replication before stopping net thread
		net->SetReplicationSystem(nullptr);
		if (ServerFlow&& ServerFlow
		->
		GetWorld()
		)
		ServerFlow->GetWorld()->SetReplicationSystem(nullptr);

		// PIENetThread is Sentinel-driven — no Stop/Join needed.
		// Connections are closed below; PumpMessages will drain remaining messages.

		NetConnectionManager* connMgr = net->GetConnectionManager();

		// Notify game code to unbind connection callbacks BEFORE closing connections
		if (EnginePtr->OnPIEStopped.IsBound())
		{
			EnginePtr->OnPIEStopped(connMgr);
		}

		// Close all PIE client connections (both sides)
		for (auto& client : PIEClients)
		{
			if (client.ServerHandle != 0) connMgr->CloseConnection(client.ServerHandle, "PIE Stop");
			if (client.ClientHandle != 0) connMgr->CloseConnection(client.ClientHandle, "PIE Stop");
		}

		// Flush GNS so it processes the connection closures before we re-listen
		connMgr->RunCallbacks();

		// Reset the OnClientConnected multicallback to prevent stale bindings
		connMgr->OnClientConnected.Reset();

		// Clear all client handler registrations
		net->ClearClients();
		net->SetAuthorityWorld(nullptr);

		connMgr->StopListening();
	}
	
	// 3. Remove viewports from renderer and free GPU resources
	renderer->WaitForGPU(); // Ensure in-flight frames finish before destroying images/descriptors
	for (auto& client : PIEClients)
	{
		renderer->RemoveViewport(client.Viewport.get());
		renderer->FreeViewportResources(client.Viewport.get());
	}
	if (ServerViewport)
	{
		renderer->RemoveViewport(ServerViewport.get());
		renderer->FreeViewportResources(ServerViewport.get());
	}

	// 4. Shutdown and destroy worlds (FlowManager destructors handle World shutdown)
	PIEClients.clear();
	Replicator.reset();
	ServerViewport.reset();
	ServerFlow.reset();

	// 5. Restore editor world to its pre-PIE sim state
	if (LogicPtr) LogicPtr->SetSimPaused(bPrePIESimWasPaused);

	bPIEActive = false;
	LOG_ENG_INFO("[PIE] Stopped");
}

void EditorContext::DrawEditorViewportPanel()
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::SetNextWindowDockID(ImGui::GetID("EditorDockspaceID"), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Viewport"))
	{
		ImVec2 panelPos  = ImGui::GetCursorScreenPos();
		ImVec2 panelSize = ImGui::GetContentRegionAvail();

		// Store for gizmo and input tracking
		ViewportPanelPos     = panelPos;
		ViewportPanelSize    = panelSize;
		ViewportPanelHovered = ImGui::IsWindowHovered();

		// Ask the renderer to resize the offscreen target if the panel changed size
		auto* renderer = static_cast<EditorRenderer*>(EnginePtr->GetRenderer());
		if (panelSize.x > 1.0f && panelSize.y > 1.0f)
		{
			renderer->ResizeEditorViewport(static_cast<uint32_t>(panelSize.x),
										   static_cast<uint32_t>(panelSize.y));
		}

		VkDescriptorSet tex = renderer->GetEditorViewportTexture();
		if (tex != VK_NULL_HANDLE && panelSize.x > 0 && panelSize.y > 0)
		{
			ImGui::Image(tex, panelSize);

			// Drag-drop target: accept prefab drops onto the viewport image
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PREFAB_PATH"))
				{
					std::string prefabPath(static_cast<const char*>(payload->Data));
					SpawnPrefab(prefabPath);
				}
				ImGui::EndDragDropTarget();
			}
		}
		DrawGizmo();
	}
	ImGui::End();
	ImGui::PopStyleVar();
}

void EditorContext::DrawViewportPanel(const char* title, WorldViewport& vp)
{
	ImGui::SetNextWindowSize(ImVec2(static_cast<float>(vp.Width), static_cast<float>(vp.Height)), ImGuiCond_Appearing);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	if (ImGui::Begin(title))
	{
		// Route input to whichever viewport panel is focused
		if (ImGui::IsWindowFocused() && vp.TargetWorld)
		{
			EnginePtr->InputTargetWorld = vp.TargetWorld;
		}

		ImVec2 panelSize = ImGui::GetContentRegionAvail();

		if (vp.ImGuiTexture != VK_NULL_HANDLE && panelSize.x > 0 && panelSize.y > 0
			&& vp.Width > 0 && vp.Height > 0)
		{
			// Fit the render target into the panel preserving aspect ratio
			float rtAspect    = static_cast<float>(vp.Width) / static_cast<float>(vp.Height);
			float panelAspect = panelSize.x / panelSize.y;

			ImVec2 imageSize;
			if (panelAspect > rtAspect)
			{
				// Panel is wider than RT — fit to height, center horizontally
				imageSize.y = panelSize.y;
				imageSize.x = panelSize.y * rtAspect;
			}
			else
			{
				// Panel is taller than RT — fit to width, center vertically
				imageSize.x = panelSize.x;
				imageSize.y = panelSize.x / rtAspect;
			}

			// Center the image within the panel
			ImVec2 cursor = ImGui::GetCursorPos();
			float offsetX = (panelSize.x - imageSize.x) * 0.5f;
			float offsetY = (panelSize.y - imageSize.y) * 0.5f;
			ImGui::SetCursorPos(ImVec2(cursor.x + offsetX, cursor.y + offsetY));

			ImGui::Image(vp.ImGuiTexture, imageSize);
		}
	}
	ImGui::End();
	ImGui::PopStyleVar();
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
										  State.CurrentScenePath.c_str(),
										  State.SceneDefaultState.empty() ? nullptr : State.SceneDefaultState.c_str(),
										  State.SceneDefaultMode.empty() ? nullptr : State.SceneDefaultMode.c_str());
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
