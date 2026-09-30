#include "EditorContext.h"
#include "EditorPanel.h"
#include "GpuFrameData.h"
#include "TnxPalette.h"
#include "TnxStyle.h"
#include "TnxWidgets.h"
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
#include "SkeletonImporter.h"
#include "SkeletonAsset.h"
#include "AnimationAsset.h"
#include "SkeletonManager.h"
#include "AnimationManager.h"
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
#include "Panels/ComponentGeneratorPanel.h"
#include "Panels/ConstructEditorWindow.h"
#include "Panels/EntityEditorWindow.h"
#include "Panels/PrefabEditorWindow.h"
#include "Panels/DebuggerPanel.h"

EditorContext::EditorContext() = default;

EditorContext::~EditorContext()
{
	SaveEditorSettings();
	if (bPIEActive) StopPIE();
	if (PrefabEditorPtr) PrefabEditorPtr->DestroyAllPreviews(State);
}

void EditorContext::Initialize(TrinyxEngine* engine, LogicThreadBase* logic)
{
	EnginePtr = engine;
	LogicPtr  = logic;

	// Populate shared state pointers for panels
	State.EnginePtr   = engine;
	State.RegistryPtr = engine->GetRegistry();
	State.ConfigPtr   = engine->GetConfig();
	State.LogicPtr    = logic;
	State.EditorCtx   = this;

	// Initialize asset database from project content directory
	const EngineConfig* cfg = engine->GetConfig();
	if (cfg->ProjectDir[0] != '\0')
	{
		std::string contentDir = std::string(cfg->ProjectDir) + "/content";
		AssetDB.Initialize(contentDir.c_str());
		State.AssetDB = &AssetDB;

		// Register demand loaders — assets load on first Checkout(), not at boot.
		AssetRegistry::Get().RegisterLoader(
			AssetType::Mesh,
			[](void*, AssetID id)
		{
			MeshManager::Get().LoadMesh(id);
		},
			nullptr);
		AssetRegistry::Get().RegisterLoader(
			AssetType::Skeleton,
			[](void*, AssetID id)
		{
			SkeletonManager::Get().LoadSkeleton(id);
		},
			nullptr);
		AssetRegistry::Get().RegisterLoader(
			AssetType::Animation,
			[](void*, AssetID id)
		{
			AnimationManager::Get().LoadAnimation(id);
		},
			nullptr);
		AssetRegistry::Get().RegisterLoader(
			AssetType::Audio,
			[](void*, AssetID id)
		{
			AudioManager::Get().LoadSound(id);
		},
			nullptr);

		CheckForAssetIssues();
		LoadEditorSettings();
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
	OutlinerPanelPtr = AddPanel<WorldOutlinerPanel>();
	DetailsPanelPtr  = AddPanel<DetailsPanel>();
	AddPanel<EngineStatsPanel>();
	AddPanel<LogPanel>();
	AddPanel<ContentBrowserPanel>();
	AddPanel<ComponentGeneratorPanel>();
	ConstructEditorPtr = AddPanel<ConstructEditorWindow>();
	EntityEditorPtr    = AddPanel<EntityEditorWindow>();
	PrefabEditorPtr    = AddPanel<PrefabEditorWindow>();
	AddPanel<DebuggerPanel>();

	LOG_ENG_INFO_F("[Editor] Initialized with %zu panels", Panels.size());

	// --- Register command palette commands ---
	TnxPalette::Clear();

	// Workspace switch × 6
	TnxPalette::Register({ "Switch to Layout workspace", nullptr, nullptr, nullptr, [this]
	{
		CurrentWorkspace = Workspace::Layout;
	} });
	TnxPalette::Register({ "Switch to Logic workspace", nullptr, nullptr, nullptr, [this]
	{
		CurrentWorkspace = Workspace::Logic;
	} });
	TnxPalette::Register({ "Switch to Simulate workspace", nullptr, nullptr, nullptr, [this]
	{
		CurrentWorkspace = Workspace::Simulate;
	} });
	TnxPalette::Register({ "Switch to Network workspace", nullptr, nullptr, nullptr, [this]
	{
		CurrentWorkspace = Workspace::Network;
	} });
	TnxPalette::Register({ "Switch to Profile workspace", nullptr, nullptr, nullptr, [this]
	{
		CurrentWorkspace = Workspace::Profile;
	} });
	TnxPalette::Register({ "Switch to Asset workspace", nullptr, nullptr, nullptr, [this]
	{
		CurrentWorkspace = Workspace::Asset;
	} });

	// Scene operations
	TnxPalette::Register({ "Open Scene…", nullptr, nullptr, "Ctrl+O", [this]
	{
		bShowFileDialog    = true;
		bFileDialogForSave = false;
		FileDialogPath     = State.CurrentScenePath;
	} });
	TnxPalette::Register({ "Save Scene", nullptr, nullptr, "Ctrl+S", [this]
	{
		if (!State.CurrentScenePath.empty())
		{
			EntityBuilder::SaveToFile(State.RegistryPtr, State.CurrentSceneName.c_str(),
				State.CurrentScenePath.c_str(),
				State.SceneDefaultState.empty() ? nullptr : State.SceneDefaultState.c_str(),
				State.SceneDefaultMode.empty() ? nullptr : State.SceneDefaultMode.c_str());
			State.bSceneDirty = false;
		}
	} });
	TnxPalette::Register({ "Save Scene As…", nullptr, nullptr, "Ctrl+Shift+S", [this]
	{
		bShowFileDialog    = true;
		bFileDialogForSave = true;
		FileDialogPath     = State.CurrentScenePath;
	} });

	// PIE controls
	TnxPalette::Register({ "Start PIE (Local)", "Play in editor — solo world", nullptr, nullptr, [this]
	{
		if (!bPIEActive) StartPIELocal();
	} });
	TnxPalette::Register({ "Stop PIE", "End play-in-editor session", nullptr, nullptr, [this]
	{
		if (bPIEActive) bPIEStopRequested = true;
	} });

	// PIE networked modes
	TnxPalette::Register({ "PIE — 2 Owners", "Authority + 2 Owners", nullptr, nullptr, [this]
	{
		if (!bPIEActive)
		{
			PIEClientCount = 2;
			bServerVisible = true;
			StartPIE();
		}
	} });
	TnxPalette::Register({ "PIE — 3 Owners", "Authority + 3 Owners", nullptr, nullptr, [this]
	{
		if (!bPIEActive)
		{
			PIEClientCount = 3;
			bServerVisible = true;
			StartPIE();
		}
	} });
	TnxPalette::Register({ "PIE — 4 Owners", "Authority + 4 Owners", nullptr, nullptr, [this]
	{
		if (!bPIEActive)
		{
			PIEClientCount = 4;
			bServerVisible = true;
			StartPIE();
		}
	} });

	// Layout reset
	TnxPalette::Register({ "Reset Layout", "Restore default panel arrangement", nullptr, nullptr, [this]
	{
		for (auto& b : bWorkspaceLayoutBuilt)
			b = false;
		for (auto& panel : Panels)
			panel->bForceMainViewport = true;
	} });

	// Import mesh
	TnxPalette::Register({ "Import Mesh…", "Load glTF/GLB into content", nullptr, nullptr, [this]
	{
		bShowImportDialog = true;
		ImportDialogPath.clear();
	} });

	// Undo / Redo
	TnxPalette::Register({ "Undo", nullptr, nullptr, "Ctrl+Z", [this]
	{
		Undo();
	} });
	TnxPalette::Register({ "Redo", nullptr, nullptr, "Ctrl+Y", [this]
	{
		Redo();
	} });

	// Gizmo mode
	TnxPalette::Register({ "Gizmo — Translate", nullptr, nullptr, "W", [this]
	{
		State.CurrentGizmoOp = EditorState::GizmoOp::Translate;
	} });
	TnxPalette::Register({ "Gizmo — Rotate", nullptr, nullptr, "E", [this]
	{
		State.CurrentGizmoOp = EditorState::GizmoOp::Rotate;
	} });
	TnxPalette::Register({ "Gizmo — Scale", nullptr, nullptr, "R", [this]
	{
		State.CurrentGizmoOp = EditorState::GizmoOp::Scale;
	} });
	TnxPalette::Register({ "Toggle World / Local gizmo space", nullptr, nullptr, nullptr, [this]
	{
		State.bGizmoWorldMode = !State.bGizmoWorldMode;
	} });
	TnxPalette::Register({ "Toggle Gizmo snap", nullptr, nullptr, nullptr, [this]
	{
		State.bGizmoSnap = !State.bGizmoSnap;
	} });

	// Demo / debug
	TnxPalette::Register({ "Show ImGui Demo Window", nullptr, nullptr, nullptr, [this]
	{
		bShowDemoWindow = !bShowDemoWindow;
	} });
	TnxPalette::Register({ "Show ImGui Metrics", nullptr, nullptr, nullptr, [this]
	{
		bShowMetrics = !bShowMetrics;
	} });
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
		if (rootPtr->Find("entities"))
			EntityBuilder::SpawnScene(spawnReg, *rootPtr);
		else
			EntityBuilder::SpawnEntity(spawnReg, *rootPtr);
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
	TemporalFrameHeader* hdr = tc->GetFrameHeader(tc->GetActiveReadFrame());
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

	// Rebuild view and projection matrices from the viewport's resolved camera.
	// ImGuizmo expects column-major, OpenGL-style (no Vulkan Y-flip in projection).
	const ViewCamera cam = ResolveViewCamera(EnginePtr->GetRenderer()->GetEditorViewport(), *hdr);
	const Quatf camRot   = cam.Rotation;
	const float crx = camRot.x, cry = camRot.y, crz = camRot.z, crw = camRot.w;

	// quatRotate(q, v): right = q*(1,0,0), up = q*(0,1,0), fwd = q*(0,0,-1)
	auto qr = [&](float vx, float vy, float vz, float& ox, float& oy, float& oz)
	{
		float tx = 2.0f * (cry * vz - crz * vy);
		float ty = 2.0f * (crz * vx - crx * vz);
		float tz = 2.0f * (crx * vy - cry * vx);
		ox       = vx + crw * tx + (cry * tz - crz * ty);
		oy       = vy + crw * ty + (crz * tx - crx * tz);
		oz       = vz + crw * tz + (crx * ty - cry * tx);
	};

	float rx, ry, rz, ux, uy, uz, fx, fy, fz;
	qr(1, 0, 0, rx, ry, rz);  // right
	qr(0, 1, 0, ux, uy, uz);  // up
	qr(0, 0, -1, fx, fy, fz); // forward (-Z)

	const float cpx = cam.Position[0];
	const float cpy = cam.Position[1];
	const float cpz = cam.Position[2];

	// Column-major view matrix
	Matrix4f viewFixup;
	viewFixup[0]  = rx;
	viewFixup[1]  = ux;
	viewFixup[2]  = -fx;
	viewFixup[3]  = 0;
	viewFixup[4]  = ry;
	viewFixup[5]  = uy;
	viewFixup[6]  = -fy;
	viewFixup[7]  = 0;
	viewFixup[8]  = rz;
	viewFixup[9]  = uz;
	viewFixup[10] = -fz;
	viewFixup[11] = 0;
	viewFixup[12] = -(rx * cpx + ry * cpy + rz * cpz);
	viewFixup[13] = -(ux * cpx + uy * cpy + uz * cpz);
	viewFixup[14] = (fx * cpx + fy * cpy + fz * cpz);
	viewFixup[15] = 1;

	// Column-major projection (OpenGL-style, no Y-flip — ImGuizmo adds its own)
	const float aspect = (ViewportPanelSize.y > 0.f) ? ViewportPanelSize.x / ViewportPanelSize.y : 1.0f;
	const float fovRad = cam.FoVDeg * 3.14159265f / 180.0f;
	const float F      = 1.0f / std::tan(fovRad * 0.5f);
	const float zNear = 0.1f, zFar = 5000.0f;
	const float dz = zNear - zFar;

	Matrix4f projFixup;
	for (int i = 0; i < 16; ++i)
		projFixup[i] = 0.0f;
	projFixup[0]  = F / aspect;
	projFixup[5]  = F; // Y-up (no Vulkan flip)
	projFixup[10] = zFar / dz;
	projFixup[11] = -1.0f;
	projFixup[14] = (zFar * zNear) / dz;

	// Map our enum to ImGuizmo operation
	ImGuizmo::OPERATION op;
	switch (State.CurrentGizmoOp)
	{
		case EditorState::GizmoOp::Translate:
			op = ImGuizmo::TRANSLATE;
			break;
		case EditorState::GizmoOp::Rotate:
			op = ImGuizmo::ROTATE;
			break;
		case EditorState::GizmoOp::Scale:
			op = ImGuizmo::SCALE;
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
			case EditorState::GizmoOp::Translate:
				snapVal = State.GizmoSnapTranslate;
				break;
			case EditorState::GizmoOp::Rotate:
				snapVal = State.GizmoSnapRotate;
				break;
			case EditorState::GizmoOp::Scale:
				snapVal = State.GizmoSnapScale;
				break;
		}
		snapValues[0] = snapValues[1] = snapValues[2] = snapVal;
		snapPtr                                       = snapValues;
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
			*pRotQw = SimFloat(cx * cy * cz + sx2 * sy2 * sz2);
			*pRotQx = SimFloat(sx2 * cy * cz - cx * sy2 * sz2);
			*pRotQy = SimFloat(cx * sy2 * cz + sx2 * cy * sz2);
			*pRotQz = SimFloat(cx * cy * sz2 - sx2 * sy2 * cz);
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
				auto* cache = State.RegistryPtr->GetTemporalCache();
				auto* flags = reinterpret_cast<int32_t*>(cache->GetWriteFramePtr(base));
				flags[State.SelectedLocalIndex] |= static_cast<int32_t>(TemporalFlagBits::Dirty)
												   | static_cast<int32_t>(TemporalFlagBits::DirtiedFrame);
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

		// Pick target is at logical pixel resolution (matches ImGui panelSize) so
		// coordinates must be logical too — no DPI scaling.
		int32_t pickX = static_cast<int32_t>(mousePos.x - ViewportPanelPos.x);
		int32_t pickY = static_cast<int32_t>(mousePos.y - ViewportPanelPos.y);

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

	// Refresh replication system pointer — valid only during PIE, null otherwise.
	{
		WorldBase* serverWorld = (bPIEActive && ServerFlow) ? ServerFlow->GetWorld() : nullptr;
		State.ReplicatorPtr    = serverWorld ? serverWorld->GetReplicationSystem() : nullptr;
	}

	// Main editor scene viewport — always visible, dockable
	DrawEditorViewportPanel();

	// Consume GPU pick results — must run after DrawEditorViewportPanel so
	// ViewportPanelHovered and ImGuizmo::IsOver() reflect the current frame.
	ConsumePick();

	// Draw all panels
	for (auto& panel : Panels)
	{
		if (panel->bVisible && WorkspaceMainNode != 0 && !IsInWorkspace(panel->Title))
			ImGui::SetNextWindowDockID(WorkspaceMainNode, ImGuiCond_Appearing);
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
#ifdef TNX_DEBUG_RENDERING
		if (ImGui::IsKeyPressed(ImGuiKey_F1) && !io.KeyShift) State.DebugDrawMode = GpuDebugMode_Off;
		if (ImGui::IsKeyPressed(ImGuiKey_F2)) State.DebugDrawMode = GpuDebugMode_SkinPath;
#endif
	}

	// Modals / overlays
	DrawFileDialog();
	DrawImportDialog();
	DrawUnsavedWarning();
	DrawPrefabSaveDialog();
	DrawAssetIssuesDialog();

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

	// Always-visible frame budget strip anchored to bottom-right corner
	DrawFrameBudgetOverlay();

	// Command palette overlay (Ctrl+K)
	TnxPalette::Draw();

	// Tell Sentinel whether the engine should own input — only while Play is running.
	bool playing = bPIEActive && !bPIEPaused;
	// Escape requests PIE stop — deferred to after the ImGui frame completes
	// so we don't free GPU resources (descriptor sets, images) mid-frame.
	if (bPIEActive && ImGui::IsKeyPressed(ImGuiKey_Escape)) bPIEStopRequested = true;

	// Shift+F1 toggles mouse between engine and editor during PIE/Play.
	// When released, editor gets mouse for panel interaction; re-press to return control.
	if (playing && ImGui::IsKeyPressed(ImGuiKey_F1) && io.KeyShift) bMouseReleasedDuringPlay = !bMouseReleasedDuringPlay;
	if (!playing) bMouseReleasedDuringPlay = false;
	bool engineGetsInput = playing && !bMouseReleasedDuringPlay;
	EnginePtr->Render->SetEditorOwnsKeyboard(!engineGetsInput);

	UpdateEditorCamera(engineGetsInput);
}

void EditorContext::UpdateEditorCamera(bool engineGetsInput)
{
	EditorRenderer* renderer = EnginePtr->GetRenderer();
	WorldViewport& vp        = renderer->GetEditorViewport();
	vp.bUseEditorCamera      = true;

	// Adopt the world's camera once, so the first view matches what the level published.
	if (!vp.Camera.IsSeeded() && State.RegistryPtr)
	{
		ComponentCacheBase* tc = State.RegistryPtr->GetTemporalCache();
		if (const TemporalFrameHeader* hdr = tc->GetFrameHeader(tc->GetActiveReadFrame())) vp.Camera.SeedFrom(*hdr);
	}

	// Fly starts on a right-press over the viewport and lasts until release, even if the
	// (hidden, relative-mode) cursor would have left the panel.
	const bool startFly = ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ViewportPanelHovered;
	const bool holdFly  = bEditorCameraFlying && ImGui::IsMouseDown(ImGuiMouseButton_Right);
	bEditorCameraFlying = !engineGetsInput && (startFly || holdFly);

	renderer->SetEditorCapturesMouse(bEditorCameraFlying);
	if (!bEditorCameraFlying) return;

	const ImGuiIO& io = ImGui::GetIO();
	EditorCameraInput input;
	input.MouseDX = renderer->GetMouseRelX();
	input.MouseDY = renderer->GetMouseRelY();
	input.Wheel   = renderer->GetMouseWheel();
	input.Forward = ImGui::IsKeyDown(ImGuiKey_W);
	input.Back    = ImGui::IsKeyDown(ImGuiKey_S);
	input.Left    = ImGui::IsKeyDown(ImGuiKey_A);
	input.Right   = ImGui::IsKeyDown(ImGuiKey_D);
	input.Up      = ImGui::IsKeyDown(ImGuiKey_E);
	input.Down    = ImGui::IsKeyDown(ImGuiKey_Q);
	input.Boost   = io.KeyShift;
	vp.Camera.Fly(input, io.DeltaTime);
}

void EditorContext::DrawFrameBudgetOverlay()
{
	if (!State.LogicPtr) return;

	const float logicFps = State.LogicPtr->GetLogicFPS();
	const float fixedFps = State.LogicPtr->GetFixedFPS();
	const float fixedMs  = State.LogicPtr->GetFixedFrameMs();
	if (logicFps < 0.5f) return; // not running yet

	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImVec2 overlayPos(vp->WorkPos.x + vp->WorkSize.x - 220.0f,
		vp->WorkPos.y + vp->WorkSize.y - 38.0f);

	ImGui::SetNextWindowPos(overlayPos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(210.0f, 28.0f), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.72f);

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
							 | ImGuiWindowFlags_NoDocking
							 | ImGuiWindowFlags_NoInputs
							 | ImGuiWindowFlags_NoMove
							 | ImGuiWindowFlags_NoNav
							 | ImGuiWindowFlags_NoSavedSettings;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, TnxStyle::Color::BgApp);

	if (ImGui::Begin("##FrameBudgetOverlay", nullptr, flags))
	{
		ImGui::PushFont(TnxStyle::Font::MonoBold);
		ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);

		char buf[80];
		snprintf(buf, sizeof(buf), "Brain %.2fms  Fixed %.0fHz  UI %.0fHz",
			fixedMs, fixedFps, logicFps);
		ImGui::TextUnformatted(buf);

		ImGui::PopStyleColor();
		ImGui::PopFont();
	}
	ImGui::End();

	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
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
	ImGui::SetNextWindowViewport(viewport->ID);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

	ImGui::Begin("EditorDockspace", nullptr, windowFlags);
	ImGui::PopStyleVar(3);

	ImGuiID dockspaceID = ImGui::GetID("EditorDockspaceID");
	ImGui::DockSpace(dockspaceID, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

	// Apply layout on first frame, workspace switch, or after a Reset Layout
	int wsIdx = static_cast<int>(CurrentWorkspace);
	if (bFirstFrame || CurrentWorkspace != LastAppliedWorkspace || !bWorkspaceLayoutBuilt[wsIdx])
	{
		ApplyWorkspaceLayout(dockspaceID, CurrentWorkspace);
		bWorkspaceLayoutBuilt[wsIdx] = true;
		LastAppliedWorkspace         = CurrentWorkspace;
		bFirstFrame                  = false;
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

	// Bottom: tabbed — Content Browser, Log, Engine Stats, Node Script, Component Generator, Debugger
	ImGui::DockBuilderDockWindow("Content Browser", bottom);
	ImGui::DockBuilderDockWindow("Log", bottom);
	ImGui::DockBuilderDockWindow("Engine Stats", bottom);
	ImGui::DockBuilderDockWindow("Node Script", bottom);
	ImGui::DockBuilderDockWindow("Component Generator", bottom);
	ImGui::DockBuilderDockWindow("Debugger", bottom);

	ImGui::DockBuilderFinish(dockspaceID);
}

void EditorContext::ApplyWorkspaceLayout(unsigned int dockspaceID, Workspace ws)
{
	ImGui::DockBuilderRemoveNode(dockspaceID);
	ImGui::DockBuilderAddNode(dockspaceID, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(dockspaceID, ImGui::GetMainViewport()->WorkSize);

	ImGuiID top, bottom, left, right, center, centerRight;

	// Track which windows this workspace places, and where the Viewport lands — windows opened
	// later (View menu, PIE viewports) dock there instead of floating.
	WorkspaceWindows.clear();
	WorkspaceMainNode = 0;
	auto dock         = [this](const char* name, ImGuiID node)
	{
		ImGui::DockBuilderDockWindow(name, node);
		WorkspaceWindows.emplace_back(name);
		if (std::strcmp(name, "Viewport") == 0) WorkspaceMainNode = node;
	};

	switch (ws)
	{
		case Workspace::Layout:
		default:
		{
			ImGui::DockBuilderSplitNode(dockspaceID, ImGuiDir_Down, 0.25f, &bottom, &top);
			ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, 0.15f, &left, &centerRight);
			ImGui::DockBuilderSplitNode(centerRight, ImGuiDir_Right, 0.25f, &right, &center);

			dock("World Outliner", left);
			dock("Viewport", center);
			dock("Details", right);
			dock("Content Browser", bottom);
			dock("Log", bottom);
			dock("Engine Stats", bottom);
			dock("Component Generator", bottom);
			dock("Debugger", bottom);
			break;
		}
		case Workspace::Logic:
		{
			ImGuiID leftNarrow, mainArea, mainRight, mainBottom;
			ImGui::DockBuilderSplitNode(dockspaceID, ImGuiDir_Left, 0.12f, &leftNarrow, &mainArea);
			ImGui::DockBuilderSplitNode(mainArea, ImGuiDir_Right, 0.25f, &mainRight, &mainArea);
			ImGui::DockBuilderSplitNode(mainArea, ImGuiDir_Down, 0.22f, &mainBottom, &mainArea);

			dock("World Outliner", leftNarrow);
			dock("Viewport", mainArea);
			dock("Construct Editor", mainArea);
			dock("Entity Editor", mainArea);
			dock("Component Generator", mainRight);
			dock("Details", mainRight);
			dock("Log", mainBottom);
			dock("Content Browser", mainBottom);
			dock("Engine Stats", mainBottom);
			dock("Debugger", mainBottom);
			break;
		}
		case Workspace::Simulate:
		{
			ImGuiID bottomStrip, mainArea2, inspRight;
			ImGui::DockBuilderSplitNode(dockspaceID, ImGuiDir_Down, 0.22f, &bottomStrip, &mainArea2);
			ImGui::DockBuilderSplitNode(mainArea2, ImGuiDir_Right, 0.24f, &inspRight, &mainArea2);

			dock("Viewport", mainArea2);
			dock("Details", inspRight);
			dock("World Outliner", inspRight);
			dock("Log", bottomStrip);
			dock("Engine Stats", bottomStrip);
			dock("Debugger", bottomStrip);
			dock("Content Browser", bottomStrip);
			dock("Component Generator", bottomStrip);
			break;
		}
		case Workspace::Network:
		{
			ImGuiID netBottom, netRight, netCenter;
			ImGui::DockBuilderSplitNode(dockspaceID, ImGuiDir_Down, 0.25f, &netBottom, &netCenter);
			ImGui::DockBuilderSplitNode(netCenter, ImGuiDir_Right, 0.28f, &netRight, &netCenter);

			dock("Viewport", netCenter);
			dock("Debugger", netRight);
			dock("Details", netRight);
			dock("Log", netBottom);
			dock("Engine Stats", netBottom);
			dock("World Outliner", netBottom);
			dock("Content Browser", netBottom);
			dock("Component Generator", netBottom);
			break;
		}
		case Workspace::Profile:
		{
			ImGuiID profLeft, profRight, profBottom, profCenter2;
			ImGui::DockBuilderSplitNode(dockspaceID, ImGuiDir_Down, 0.28f, &profBottom, &profRight);
			ImGui::DockBuilderSplitNode(profRight, ImGuiDir_Left, 0.28f, &profLeft, &profCenter2);

			dock("Engine Stats", profLeft);
			dock("World Outliner", profLeft);
			dock("Viewport", profCenter2);
			dock("Debugger", profCenter2);
			dock("Log", profBottom);
			dock("Details", profBottom);
			dock("Content Browser", profBottom);
			dock("Component Generator", profBottom);
			break;
		}
		case Workspace::Asset:
		{
			ImGuiID assetBottom, assetMain;
			ImGui::DockBuilderSplitNode(dockspaceID, ImGuiDir_Down, 0.22f, &assetBottom, &assetMain);

			dock("Viewport", assetMain);
			dock("Prefab Editor", assetMain);
			dock("Content Browser", assetBottom);
			dock("Log", assetBottom);
			break;
		}
	}

	ImGui::DockBuilderFinish(dockspaceID);

	// Windows left over from the previous workspace would otherwise float at their old dock size.
	for (auto& panel : Panels)
		panel->bVisible = IsInWorkspace(panel->Title);
}

bool EditorContext::IsInWorkspace(const char* windowName) const
{
	for (const std::string& name : WorkspaceWindows)
		if (name == windowName) return true;
	return false;
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
		if (ImGui::MenuItem("Import Mesh...", nullptr, false, State.ConfigPtr != nullptr))
		{
			bShowImportDialog = true;
			ImportDialogPath.clear();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Exit")) EnginePtr->RequestExit();
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
			for (auto& b : bWorkspaceLayoutBuilt)
				b = false;
			for (auto& panel : Panels)
				panel->bForceMainViewport = true;
		}
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Play"))
	{
		if (ImGui::MenuItem("Play (Local)", nullptr, false, !bPIEActive))
		{
			StartPIELocal();
		}

		ImGui::Separator();

		ImGui::SetNextItemWidth(150);
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
		if (ImGui::MenuItem(bPIEPaused ? "Resume PIE" : "Pause PIE", nullptr, false, bPIEActive))
		{
			bPIEPaused      = !bPIEPaused;
			auto applyPause = [&](FlowManagerBase* flow)
			{
				if (!flow) return;
				WorldBase* w = flow->GetWorld();
				if (w && w->GetLogicThread()) w->GetLogicThread()->SetSimPaused(bPIEPaused);
			};
			if (bPIELocalMode)
			{
				applyPause(LocalPIEFlow.get());
			}
			else
			{
				applyPause(ServerFlow.get());
				for (auto& c : PIEClients)
					applyPause(c.Flow.get());
			}
		}

		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Debug"))
	{
		ImGui::MenuItem("Show Demo Window", nullptr, &bShowDemoWindow);
		ImGui::MenuItem("Show ImGui Metrics", nullptr, &bShowMetrics);
		ImGui::EndMenu();
	}


	ImGui::Separator();

	// --- Workspace pills ---
	{
		static constexpr const char* kWorkspaceNames[] = {
			"Layout", "Logic", "Simulate", "Network", "Profile", "Asset"
		};
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 5.0f));
		for (int i = 0; i < static_cast<int>(Workspace::COUNT); ++i)
		{
			bool active = CurrentWorkspace == static_cast<Workspace>(i);
			if (active)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, TnxStyle::Color::Purple);
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, TnxStyle::Color::PurpleHot);
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, TnxStyle::Color::PurpleSoft);
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
				ImGui::PushFont(TnxStyle::Font::UiSemibold);
			}
			else
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, TnxStyle::Color::BgElev);
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, TnxStyle::Color::BgElev);
				ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);
			}
			if (ImGui::Button(kWorkspaceNames[i]))
			{
				CurrentWorkspace = static_cast<Workspace>(i);
				// Layout will be applied on next frame if not already built
				// (bWorkspaceLayoutBuilt[i] stays true if user has already visited)
			}
			if (active) ImGui::PopFont();
			ImGui::PopStyleColor(4);
			ImGui::SameLine(0.0f, 4.0f);
		}
		ImGui::PopStyleVar(2);

		ImGui::Dummy(ImVec2(8.0f, 0.0f));
		ImGui::SameLine();
	}

	// Window drag handle
	{
		float rightX = ImGui::GetWindowWidth() - 430.0f;
		float dragW  = rightX - ImGui::GetCursorPosX() - 4.0f;
		if (dragW > 1.0f)
		{
			ImGui::InvisibleButton("##windrag", ImVec2(dragW, ImGui::GetFrameHeight()));
			if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
			if (ImGui::IsItemActive())
			{
				ImVec2 d = ImGui::GetIO().MouseDelta;
				if (d.x != 0.0f || d.y != 0.0f) EnginePtr->DeferWindowMove(d.x, d.y);
			}
			ImGui::SameLine(0.0f, 0.0f);
		}
	}

	// --- PIE controls — mode selector + client count + play/pause/stop ---
	{
		// Right-anchor the group; leave room for mode combo (~160) + clients (~55) + buttons (~200)
		float rightX = ImGui::GetWindowWidth() - 430.0f;
		if (ImGui::GetCursorPosX() < rightX)
			ImGui::SetCursorPosX(rightX);

		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

		if (!bPIEActive)
		{
			// Mode selector combo
			static constexpr const char* kPIEModeLabels[] = {
				"STANDALONE", "LISTEN + CLIENTS", "DEDICATED + CLIENTS"
			};
			int modeIdx = static_cast<int>(CurrentPIEMode);
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, TnxStyle::Color::BgElev);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, TnxStyle::Color::BgElev);
			ImGui::PushStyleColor(ImGuiCol_FrameBg, TnxStyle::Color::BgDeep);
			ImGui::PushFont(TnxStyle::Font::MonoRegular ? TnxStyle::Font::MonoRegular : ImGui::GetFont());
			ImGui::SetNextItemWidth(160.0f);
			if (ImGui::BeginCombo("##piemode", kPIEModeLabels[modeIdx], ImGuiComboFlags_NoArrowButton))
			{
				for (int i = 0; i < 3; ++i)
				{
					bool sel = (modeIdx == i);
					if (ImGui::Selectable(kPIEModeLabels[i], sel))
						CurrentPIEMode = static_cast<PIEMode>(i);
					if (sel) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			ImGui::PopFont();
			ImGui::PopStyleColor(4);
			ImGui::SameLine(0.0f, 4.0f);

			// Client count — only shown for networked modes
			if (CurrentPIEMode != PIEMode::Local)
			{
				ImGui::PushFont(TnxStyle::Font::MonoRegular ? TnxStyle::Font::MonoRegular : ImGui::GetFont());
				ImGui::SetNextItemWidth(45.0f);
				ImGui::InputInt("##pieclientcount", &PIEClientCount, 0, 0);
				if (PIEClientCount < 1) PIEClientCount = 1;
				if (PIEClientCount > 4) PIEClientCount = 4;
				ImGui::PopFont();
				ImGui::SameLine(0.0f, 4.0f);
			}

			// Play — yellow, dark text
			ImGui::PushStyleColor(ImGuiCol_Button, TnxStyle::Color::Yellow);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, TnxStyle::Color::YellowHot);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, TnxStyle::Color::YellowSoft);
			ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::YellowOnYellow);
			if (ImGui::Button("  Play  "))
			{
				switch (CurrentPIEMode)
				{
					case PIEMode::Local: StartPIELocal(); break;
					case PIEMode::ListenServer:
						bServerVisible = true;
						StartPIE();
						break;
					case PIEMode::HeadlessServer:
						bServerVisible = false;
						StartPIE();
						break;
				}
			}
			ImGui::PopStyleColor(4);
		}
		else
		{
			// Pause / Resume
			if (!bPIEPaused)
			{
				if (ImGui::Button("  Pause ") && State.LogicPtr)
				{
					bPIEPaused = true;
					State.LogicPtr->SetSimPaused(true);
				}
			}
			else
			{
				// Resume — yellow tint, dark text so it reads on both YellowSoft and Yellow
				ImGui::PushStyleColor(ImGuiCol_Button, TnxStyle::Color::YellowSoft);
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, TnxStyle::Color::Yellow);
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, TnxStyle::Color::YellowHot);
				ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::YellowOnYellow);
				if (ImGui::Button(" Resume ") && State.LogicPtr)
				{
					bPIEPaused = false;
					State.LogicPtr->SetSimPaused(false);
				}
				ImGui::PopStyleColor(4);
			}

			ImGui::SameLine(0.0f, 4.0f);

			// Stop
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.60f, 0.18f, 0.18f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.22f, 0.22f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.50f, 0.15f, 0.15f, 1.0f));
			if (ImGui::Button("  Stop  ")) bPIEStopRequested = true;
			ImGui::PopStyleColor(3);
		}

		ImGui::PopStyleVar();
	}

	// Scene name + window controls (right side of menu bar)
	{
		constexpr float kBtnW = 36.0f;
		const float totalBtnW = kBtnW * 3.0f;

		char sceneLabel[256];
		snprintf(sceneLabel, sizeof(sceneLabel), "%s%s",
			State.bSceneDirty ? "* " : "",
			State.CurrentSceneName.c_str());

		float textWidth = ImGui::CalcTextSize(sceneLabel).x;
		float btnX      = ImGui::GetWindowWidth() - totalBtnW;
		float labelX    = btnX - ImGui::GetStyle().ItemSpacing.x - textWidth;
		if (labelX > ImGui::GetCursorPosX())
		{
			ImGui::SetCursorPosX(labelX);
			ImGui::TextDisabled("%s", sceneLabel);
		}
		ImGui::SetCursorPosX(btnX);

		// SDL window ops must run on the main thread; queue them for PumpEvents.
		SDL_Window* win = EnginePtr->GetWindow();
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.15f));

		if (ImGui::Button("_##wmin", ImVec2(kBtnW, 0.0f))) EnginePtr->DeferWindowOp(1);
		ImGui::SameLine(0.0f, 0.0f);

		bool bMaximized = (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED) != 0;
		if (ImGui::Button(bMaximized ? "-##wmax" : "+##wmax", ImVec2(kBtnW, 0.0f)))
			EnginePtr->DeferWindowOp(2);
		ImGui::SameLine(0.0f, 0.0f);

		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.10f, 0.10f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.00f, 0.20f, 0.20f, 1.0f));
		if (ImGui::Button("X##wclose", ImVec2(kBtnW, 0.0f))) EnginePtr->RequestExit();
		ImGui::PopStyleColor(5);
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
			if (FileDialogPath.find(contentDir) == 0)
				relativePath = FileDialogPath.substr(contentDir.length());
			else
				relativePath = FileDialogPath;
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

void EditorContext::CheckForAssetIssues()
{
	AssetRegistry::Get().ValidateAll();

	AssetIssues.clear();
	const std::string contentBase = State.ConfigPtr
										? std::string(State.ConfigPtr->ProjectDir) + "/content/"
										: std::string{};

	for (const auto& [id, entry] : AssetRegistry::Get().GetAllEntries())
	{
		const bool missing = (static_cast<uint8_t>(entry.State)
								 & static_cast<uint8_t>(RuntimeFlags::Missing))
							 != 0;
		if (!missing) continue;

		AssetIssue issue;
		issue.ID             = id;
		issue.Name           = entry.Name.GetStr();
		issue.RegisteredPath = entry.Path;

		// Infer source path from the registered asset extension.
		if (!contentBase.empty() && !entry.Path.empty())
		{
			std::filesystem::path p(entry.Path);
			std::string stem = (std::filesystem::path(contentBase) / p.parent_path() / p.stem()).string();
			for (const char* ext : { ".gltf", ".glb" })
			{
				if (std::filesystem::exists(stem + ext))
				{
					issue.SuggestedSourcePath = stem + ext;
					break;
				}
			}
		}

		snprintf(issue.ReimportBuf, sizeof(issue.ReimportBuf),
			"%s", issue.SuggestedSourcePath.c_str());

		AssetIssues.push_back(std::move(issue));
	}

	if (!AssetIssues.empty())
		bShowAssetIssuesDialog = true;
}

void EditorContext::LoadEditorSettings()
{
	if (!State.ConfigPtr || State.ConfigPtr->ProjectDir[0] == '\0') return;

	std::string path = std::string(State.ConfigPtr->ProjectDir) + "/editor_settings.json";
	std::ifstream file(path);
	if (!file.is_open()) return;

	std::ostringstream ss;
	ss << file.rdbuf();
	JsonValue root = JsonParse(ss.str());
	if (!root.IsObject()) return;

	if (const JsonValue* v = root.Find("pieMode"))
	{
		const std::string& s = v->AsString();
		if (s == "ListenServer")
			CurrentPIEMode = PIEMode::ListenServer;
		else if (s == "HeadlessServer")
			CurrentPIEMode = PIEMode::HeadlessServer;
		else
			CurrentPIEMode = PIEMode::Local;
	}
	if (const JsonValue* v = root.Find("pieClientCount"))
		PIEClientCount = std::max(1, std::min(4, v->AsInt(1)));

	if (const JsonValue* v = root.Find("workspace"))
	{
		const std::string& s = v->AsString();
		if (s == "Logic")
			CurrentWorkspace = Workspace::Logic;
		else if (s == "Simulate")
			CurrentWorkspace = Workspace::Simulate;
		else if (s == "Network")
			CurrentWorkspace = Workspace::Network;
		else if (s == "Profile")
			CurrentWorkspace = Workspace::Profile;
		else if (s == "Asset")
			CurrentWorkspace = Workspace::Asset;
		else
			CurrentWorkspace = Workspace::Layout;
	}
}

void EditorContext::SaveEditorSettings()
{
	if (!State.ConfigPtr || State.ConfigPtr->ProjectDir[0] == '\0') return;

	const char* pieMode = "Local";
	switch (CurrentPIEMode)
	{
		case PIEMode::ListenServer: pieMode = "ListenServer"; break;
		case PIEMode::HeadlessServer: pieMode = "HeadlessServer"; break;
		default: break;
	}

	const char* workspace = "Layout";
	switch (CurrentWorkspace)
	{
		case Workspace::Logic: workspace = "Logic"; break;
		case Workspace::Simulate: workspace = "Simulate"; break;
		case Workspace::Network: workspace = "Network"; break;
		case Workspace::Profile: workspace = "Profile"; break;
		case Workspace::Asset: workspace = "Asset"; break;
		default: break;
	}

	JsonValue root         = JsonValue::Object();
	root["pieMode"]        = JsonValue::String(pieMode);
	root["pieClientCount"] = JsonValue::Number(PIEClientCount);
	root["workspace"]      = JsonValue::String(workspace);

	std::string path = std::string(State.ConfigPtr->ProjectDir) + "/editor_settings.json";
	std::ofstream file(path);
	if (file.is_open())
		file << JsonWrite(root, true);
}

void EditorContext::DrawAssetIssuesDialog()
{
	if (!bShowAssetIssuesDialog) return;

	ImGui::OpenPopup("Asset Issues");

	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(700, 0), ImGuiCond_Appearing);

	if (ImGui::BeginPopupModal("Asset Issues", &bShowAssetIssuesDialog, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextUnformatted("The following registered assets are missing from disk:");
		ImGui::Spacing();

		for (int i = static_cast<int>(AssetIssues.size()) - 1; i >= 0; --i)
		{
			AssetIssue& issue = AssetIssues[i];
			ImGui::PushID(i);

			ImGui::TextUnformatted(issue.Name.c_str());
			ImGui::SameLine(160);
			ImGui::TextDisabled("%s", issue.RegisteredPath.c_str());

			ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150);

			const bool canReimport = (AssetRegistry::Get().Find(issue.ID) != nullptr)
									 && (issue.ID.GetType() == AssetType::Mesh
										 || issue.ID.GetType() == AssetType::Skeleton);

			if (canReimport)
			{
				if (ImGui::Button("Reimport"))
					issue.ShowReimportInput = !issue.ShowReimportInput;
				ImGui::SameLine();
			}

			if (ImGui::Button("Remove"))
			{
				AssetDB.Remove(issue.ID);
				AssetDB.Save();
				AssetIssues.erase(AssetIssues.begin() + i);
				ImGui::PopID();
				continue;
			}

			if (issue.ShowReimportInput)
			{
				ImGui::SetNextItemWidth(-90);
				ImGui::InputText("##repath", issue.ReimportBuf, sizeof(issue.ReimportBuf));
				ImGui::SameLine();
				if (ImGui::Button("Import"))
				{
					if (ImportMeshAsset(issue.ReimportBuf) != UINT32_MAX)
					{
						AssetIssues.erase(AssetIssues.begin() + i);
						ImGui::PopID();
						continue;
					}
				}
			}

			ImGui::Separator();
			ImGui::PopID();
		}

		if (AssetIssues.empty())
		{
			bShowAssetIssuesDialog = false;
			ImGui::CloseCurrentPopup();
		}
		else
		{
			ImGui::Spacing();
			if (ImGui::Button("Dismiss", ImVec2(100, 0)))
			{
				bShowAssetIssuesDialog = false;
				ImGui::CloseCurrentPopup();
			}
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
	if (!State.ConfigPtr) return UINT32_MAX;

	std::filesystem::path src(gltfPath);
	std::string stem       = src.stem().string();
	std::string contentDir = std::string(State.ConfigPtr->ProjectDir) + "/content/";

	// Try skeletal import first — if the glTF contains skins, use the full pipeline.
	SkeletalImportResult skelResult;
	if (ImportSkeletalGLTF(gltfPath, skelResult) && skelResult.IsValid())
	{
		// Write all skeletal assets to content/
		std::string meshPath = contentDir + stem + ".tnxmesh";
		std::string skelPath = contentDir + stem + ".tnxskel";

		if (!SaveMeshAsset(skelResult.mesh, meshPath))
		{
			LOG_ENG_ERROR_F("[Editor] SaveMeshAsset failed: %s", meshPath.c_str());
			return UINT32_MAX;
		}
		if (!SaveSkeletonAsset(skelResult.skeleton, skelPath))
		{
			LOG_ENG_ERROR_F("[Editor] SaveSkeletonAsset failed: %s", skelPath.c_str());
			return UINT32_MAX;
		}
		for (size_t i = 0; i < skelResult.animations.size(); ++i)
		{
			const char* animStr  = skelResult.animNames[i].IsValid()
									   ? skelResult.animNames[i].GetStr()
									   : nullptr;
			std::string animName = animStr ? animStr : ("anim" + std::to_string(i));
			std::string animPath = contentDir + stem + "_" + animName + ".tnxanim";
			if (!SaveAnimationAsset(skelResult.animations[i], animPath))
				LOG_ENG_WARN_F("[Editor] SaveAnimationAsset failed: %s", animPath.c_str());
		}

		// Reconcile to register all new files in the asset database
		AssetDB.Reconcile();

		// Load mesh geometry + skin weights into MeshManager
		const auto* meshEntry = AssetDB.FindByPath(stem + ".tnxmesh");
		AssetID meshID        = meshEntry ? meshEntry->ID : AssetID{};
		uint32_t meshSlot     = MeshManager::Get().LoadMesh(skelResult.mesh, TnxName(stem.c_str()), meshID);
		if (meshSlot == UINT32_MAX)
		{
			LOG_ENG_ERROR_F("[Editor] MeshManager::LoadMesh failed for '%s'", stem.c_str());
			return UINT32_MAX;
		}

		// Load skeleton
		const auto* skelEntry = AssetDB.FindByPath(stem + ".tnxskel");
		AssetID skelID        = skelEntry ? skelEntry->ID : AssetID{};
		uint32_t skelSlot     = SkeletonManager::Get().LoadSkeleton(
			skelResult.skeleton, TnxName(stem.c_str()), skelID);
		if (skelSlot == UINT32_MAX)
			LOG_ENG_WARN_F("[Editor] SkeletonManager::LoadSkeleton failed: %s", stem.c_str());

		// Load animations
		for (size_t i = 0; i < skelResult.animations.size(); ++i)
		{
			const char* animStr   = skelResult.animNames[i].IsValid()
										? skelResult.animNames[i].GetStr()
										: nullptr;
			std::string animName  = animStr ? animStr : ("anim" + std::to_string(i));
			std::string relAnim   = stem + "_" + animName + ".tnxanim";
			const auto* animEntry = AssetDB.FindByPath(relAnim);
			AssetID animID        = animEntry ? animEntry->ID : AssetID{};
			uint32_t animSlot     = AnimationManager::Get().LoadAnimation(
				skelResult.animations[i], TnxName(animName.c_str()), animID);
			if (animSlot == UINT32_MAX)
				LOG_ENG_WARN_F("[Editor] AnimationManager::LoadAnimation failed: %s", animName.c_str());
		}

		LOG_ENG_INFO_F("[Editor] Imported skeletal mesh '%s' → mesh slot %u, skel slot %u, %u anim(s)",
			stem.c_str(), meshSlot, skelSlot,
			static_cast<uint32_t>(skelResult.animations.size()));
		return meshSlot;
	}

	// Fall back: static mesh import
	std::string outPath = contentDir + stem + ".tnxmesh";
	if (!ImportGLTF(gltfPath, outPath))
	{
		LOG_ENG_ERROR_F("[Editor] ImportGLTF failed: %s", gltfPath.c_str());
		return UINT32_MAX;
	}
	LOG_ENG_INFO_F("[Editor] Wrote %s", outPath.c_str());

	AssetDB.Reconcile();

	MeshAsset asset;
	if (!LoadMeshAsset(asset, outPath))
	{
		LOG_ENG_ERROR_F("[Editor] LoadMeshAsset failed: %s", outPath.c_str());
		return UINT32_MAX;
	}

	const auto* dbEntry = AssetDB.FindByPath(stem + ".tnxmesh");
	AssetID meshID      = dbEntry ? dbEntry->ID : AssetID{};
	uint32_t slot       = MeshManager::Get().LoadMesh(asset, TnxName(stem.c_str()), meshID);
	if (slot != UINT32_MAX)
		LOG_ENG_INFO_F("[Editor] Registered mesh '%s' at slot %u (AssetID: %lld)",
			stem.c_str(), slot, static_cast<long long>(meshID.GetUUID() >> 8));

	return slot;
}


void EditorContext::OpenConstructEditor(const char* typeName, const char* filePath)
{
	if (CurrentWorkspace != Workspace::Logic) CurrentWorkspace = Workspace::Logic;
	if (ConstructEditorPtr) ConstructEditorPtr->OpenConstruct(typeName, filePath);
}

void EditorContext::OpenEntityEditor(const char* typeName, const char* filePath)
{
	if (CurrentWorkspace != Workspace::Logic) CurrentWorkspace = Workspace::Logic;
	if (EntityEditorPtr) EntityEditorPtr->OpenEntity(typeName, filePath);
}

void EditorContext::OpenPrefabEditor(const std::string& filePath)
{
	if (CurrentWorkspace != Workspace::Asset) CurrentWorkspace = Workspace::Asset;
	if (PrefabEditorPtr) PrefabEditorPtr->OpenPrefab(filePath, State);
}

void EditorContext::HandleDroppedFile(const std::string& path)
{
	// Check if it's a mesh file we can import
	std::filesystem::path p(path);
	std::string ext = p.extension().string();

	// Convert extension to lowercase for comparison
	for (auto& c : ext)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

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
		if (State.ConfigPtr)
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
				TnxName dropName        = dropEntry ? dropEntry->Name : TnxName(p.stem().string().c_str());

				uint32_t slot = MeshManager::Get().LoadMesh(asset, dropName, dropID);
				if (slot != UINT32_MAX)
					LOG_ENG_INFO_F("[Editor] Loaded dropped mesh '%s' → slot %u", dropName.GetStr(), slot);
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
		if (State.ConfigPtr)
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
				TnxName name        = dbEntry ? dbEntry->Name : TnxName(stem.c_str());

				uint32_t slot = AudioManager::Get().LoadSound(outPath.c_str(), name, audioID);
				if (slot != UINT32_MAX)
					LOG_ENG_INFO_F("[Editor] Imported audio '%s' → slot %u", name.GetStr(), slot);
				else
					LOG_ENG_ERROR_F("[Editor] Failed to register imported audio: %s", outPath.c_str());
			}
		}
	}
	else if (ext == ".tnxaudio")
	{
		// Already engine format — copy to content/ and register.
		if (State.ConfigPtr)
		{
			std::string destPath = std::string(State.ConfigPtr->ProjectDir)
								   + "/content/" + p.filename().string();
			if (path != destPath) std::filesystem::copy_file(p, destPath, std::filesystem::copy_options::overwrite_existing);

			AssetDB.Reconcile();

			std::string stem    = p.stem().string();
			const auto* dbEntry = AssetDB.FindByPath(p.filename().string());
			AssetID audioID     = dbEntry ? dbEntry->ID : AssetID{};
			TnxName name        = dbEntry ? dbEntry->Name : TnxName(stem.c_str());

			uint32_t slot = AudioManager::Get().LoadSound(destPath.c_str(), name, audioID);
			if (slot != UINT32_MAX)
				LOG_ENG_INFO_F("[Editor] Loaded dropped .tnxaudio '%s' → slot %u", name.GetStr(), slot);
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
	// uint32_t cacheIndex    = State.SelectedCacheIndex;
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
			: m_Engine(engine),
			  m_Reg(reg),
			  m_ClassID(classID),
			  m_SavedState(std::move(savedState))
		{
		}

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
				Archetype::FieldKey flagKey{ CacheSlotMeta<>::StaticTypeID(), ReflectionRegistry::Get().GetCacheSlotIndex(CacheSlotMeta<>::StaticTypeID()), 0 };
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
						flagsArr[localIdx] = static_cast<int32_t>(TemporalFlagBits::Dirty)
											 | static_cast<int32_t>(TemporalFlagBits::DirtiedFrame);
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
// PIE local — single solo world, game runs in the Viewport panel
// -----------------------------------------------------------------------

void EditorContext::StartPIELocal()
{
	if (bPIEActive) return;

	LocalPIEConfig = *EnginePtr->GetGameConfig();

	LocalPIEFlow = std::make_unique<PIELocalFlow>();
	LocalPIEFlow->Initialize(EnginePtr, &LocalPIEConfig, 1280, 720);
	if (!LocalPIEFlow->CreateWorld())
	{
		LOG_ENG_ERROR("[PIE Local] Failed to create world");
		LocalPIEFlow.reset();
		return;
	}

	WorldBase* pieWorld = LocalPIEFlow->GetWorld();
	pieWorld->SetJobsInitialized(true);

	// Allocate at the current panel size — ViewportPanelSize is set by DrawEditorViewportPanel
	// on the frame before the Play button is clicked, so it's the correct stable size.
	// Allocating here at the right size avoids an immediate vkDeviceWaitIdle + realloc on the
	// first frame when ResizeViewport would otherwise fire due to the 1280x720 mismatch.
	EditorRenderer* renderer      = EnginePtr->GetRenderer();
	const uint32_t vpW            = ViewportPanelSize.x > 1.0f ? static_cast<uint32_t>(ViewportPanelSize.x) : 1280u;
	const uint32_t vpH            = ViewportPanelSize.y > 1.0f ? static_cast<uint32_t>(ViewportPanelSize.y) : 720u;
	LocalPIEViewport              = std::make_unique<WorldViewport>();
	LocalPIEViewport->TargetWorld = pieWorld;
	renderer->AllocateViewportResources(LocalPIEViewport.get(), vpW, vpH);
	renderer->AddViewport(LocalPIEViewport.get());

	// Stop rendering the editor world while PIE is running — it's not visible and running
	// two full GPU compute pipelines (predicate→scatter for editor + PIE) every frame
	// with a full pipeline barrier between them cuts throughput roughly in half.
	renderer->SetEditorViewportActive(false);

	LocalPIEFlow->StartWorld();

	if (!State.SceneDefaultMode.empty())
		LocalPIEFlow->SetGameMode(State.SceneDefaultMode.c_str());
	if (!State.SceneDefaultState.empty())
		LocalPIEFlow->LoadDefaultState(State.SceneDefaultState.c_str());

	EnginePtr->InputTargetWorld = pieWorld;

	bPrePIESimWasPaused = !LogicPtr || LogicPtr->IsSimPaused();
	if (LogicPtr) LogicPtr->SetSimPaused(true);

	bPIEPaused    = false;
	bPIELocalMode = true;
	bPIEActive    = true;
	State.ClearSelection();
	LOG_ENG_INFO("[PIE Local] Started");
}

// -----------------------------------------------------------------------
// PIE networked — server + client worlds in floating viewports
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
	for (auto& c : PIEClients)
		c.Flow->StartWorld();

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
	if (!connMgr->Listen(PIEPort, /*loopbackOnly=*/true))
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
	for (const auto& ci : connMgr->GetConnections())
		knownHandles.push_back(ci.Handle);

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
					for (uint32_t h : knownHandles)
					{
						if (h == ci.Handle)
						{
							known = true;
							break;
						}
					}
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
					if (ci.Handle == serverHandle)
					{
						ownerID = ci.OwnerID;
						break;
					}
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
		for (auto& c : PIEClients)
			c.Flow->LoadDefaultState(State.SceneDefaultState.c_str());
	}

	// 9. Default input to first client world until a viewport panel gets focus
	if (!PIEClients.empty())
	{
		EnginePtr->InputTargetWorld = PIEClients[0].Flow->GetWorld();
	}

	// 10. Pause editor world's logic thread (save state so StopPIE restores correctly)
	bPrePIESimWasPaused = !LogicPtr || LogicPtr->IsSimPaused();
	if (LogicPtr) LogicPtr->SetSimPaused(true);

	bPIEPaused = false;
	bPIEActive = true;
	State.ClearSelection();
	LOG_ENG_INFO_F("[PIE] Started: 1 server%s + %zu client(s), port %u",
		bServerVisible ? " (visible)" : " (headless)",
		PIEClients.size(), PIEPort);
}

void EditorContext::StopPIE()
{
	if (!bPIEActive) return;

	EnginePtr->InputTargetWorld = nullptr;

	EditorRenderer* renderer = EnginePtr->GetRenderer();

	if (bPIELocalMode)
	{
		// Resume if paused so the logic thread can observe the stop signal.
		if (bPIEPaused && LocalPIEFlow && LocalPIEFlow->GetWorld() && LocalPIEFlow->GetWorld()->GetLogicThread())
			LocalPIEFlow->GetWorld()->GetLogicThread()->SetSimPaused(false);

		// Stop and join the logic thread BEFORE freeing any resources it touches.
		// ~FlowManagerBase() calls ConstructReg.DestroyAll() before World::Shutdown(),
		// so without an explicit join here the logic thread can be mid-tick (EnsureHydrated,
		// HydrateAllViews) when ConstructViews and the Registry are being destroyed.
		if (LocalPIEFlow)
		{
			LocalPIEFlow->StopWorld();
			LocalPIEFlow->JoinWorld();
		}

		renderer->WaitForGPU();
		if (LocalPIEViewport)
		{
			renderer->RemoveViewport(LocalPIEViewport.get());
			renderer->FreeViewportResources(LocalPIEViewport.get());
		}

		LocalPIEViewport.reset();
		LocalPIEFlow.reset(); // logic thread already dead — safe to destroy Constructs + World

		renderer->SetEditorViewportActive(true);
		if (LogicPtr) LogicPtr->SetSimPaused(bPrePIESimWasPaused);

		bPIELocalMode = false;
		bPIEPaused    = false;
		bPIEActive    = false;
		LOG_ENG_INFO("[PIE Local] Stopped");
		return;
	}

	// 2. Tear down PIE networking
	PIENetThread* net = EnginePtr->GetNetThread();
	if (net)
	{
		// Detach replication before stopping net thread
		net->SetReplicationSystem(nullptr);
		if (ServerFlow && ServerFlow->GetWorld())
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

	// 4. Drain in-flight replication build jobs before destroying anything they point to.
	// DispatchFrameJobs() dispatches worker-pool jobs that capture raw pointers into
	// the ReplicationSystem (channels, Stats) and world slab data. Freeing those objects
	// while jobs are still running causes use-after-free writes that corrupt Jolt's
	// internal memory, producing the Jolt BodyID crash at teardown.
	if (Replicator) Replicator->WaitForBuildJobs();

	// 5. Resume any paused PIE logic threads so they can exit their fixed loop cleanly.
	if (bPIEPaused)
	{
		auto resume = [](FlowManagerBase* flow)
		{
			if (!flow) return;
			WorldBase* w = flow->GetWorld();
			if (w && w->GetLogicThread()) w->GetLogicThread()->SetSimPaused(false);
		};
		resume(ServerFlow.get());
		for (auto& c : PIEClients)
			resume(c.Flow.get());
		bPIEPaused = false;
	}

	// 6. Stop and join all PIE logic threads before destroying anything they touch.
	// ~FlowManagerBase() runs ConstructReg.DestroyAll() before World::Shutdown(), so the logic
	// thread must be fully exited before the reset calls below or EnsureHydrated/HydrateAllViews
	// can race against ConstructView and Registry teardown.
	for (auto& c : PIEClients)
	{
		if (c.Flow)
		{
			c.Flow->StopWorld();
			c.Flow->JoinWorld();
		}
	}
	if (ServerFlow)
	{
		ServerFlow->StopWorld();
		ServerFlow->JoinWorld();
	}

	// Destroy worlds — logic threads already dead, safe to tear down Constructs + Registry.
	PIEClients.clear();
	Replicator.reset();
	ServerViewport.reset();
	ServerFlow.reset();

	// 7. Restore editor world to its pre-PIE sim state
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

		ViewportPanelPos     = panelPos;
		ViewportPanelSize    = panelSize;
		ViewportPanelHovered = ImGui::IsWindowHovered();

		auto* renderer = static_cast<EditorRenderer*>(EnginePtr->GetRenderer());

		if (bPIEActive && bPIELocalMode && LocalPIEViewport)
		{
			// Show the local PIE game world in the viewport panel.
			// Resize the render target to match the panel — gated on size change.
			if (panelSize.x > 1.0f && panelSize.y > 1.0f)
			{
				renderer->ResizeViewport(LocalPIEViewport.get(),
					static_cast<uint32_t>(panelSize.x),
					static_cast<uint32_t>(panelSize.y));
			}

			if (LocalPIEViewport->ImGuiTexture != VK_NULL_HANDLE && panelSize.x > 0 && panelSize.y > 0)
			{
				ImGui::Image(LocalPIEViewport->ImGuiTexture, panelSize);

				// Route input to the PIE world when this viewport is focused
				if (ImGui::IsWindowFocused() && LocalPIEFlow)
					EnginePtr->InputTargetWorld = LocalPIEFlow->GetWorld();
			}
		}
		else
		{
			// Normal editor scene view
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
			DrawEditorGrid();
			DrawGizmo();
		}
	}
	ImGui::End();
	ImGui::PopStyleVar();
}

void EditorContext::DrawEditorGrid()
{
	if (!State.RegistryPtr) return;
	ComponentCacheBase* tc   = State.RegistryPtr->GetTemporalCache();
	TemporalFrameHeader* hdr = tc->GetFrameHeader(tc->GetActiveReadFrame());
	if (!hdr) return;
	if (ViewportPanelSize.x <= 0.0f || ViewportPanelSize.y <= 0.0f) return;

	// Camera basis (same math as DrawGizmo)
	const ViewCamera cam = ResolveViewCamera(EnginePtr->GetRenderer()->GetEditorViewport(), *hdr);
	const Quatf camRot   = cam.Rotation;
	const float crx = camRot.x, cry = camRot.y, crz = camRot.z, crw = camRot.w;
	auto qr = [&](float vx, float vy, float vz, float& ox, float& oy, float& oz)
	{
		float tx = 2.0f * (cry * vz - crz * vy);
		float ty = 2.0f * (crz * vx - crx * vz);
		float tz = 2.0f * (crx * vy - cry * vx);
		ox       = vx + crw * tx + (cry * tz - crz * ty);
		oy       = vy + crw * ty + (crz * tx - crx * tz);
		oz       = vz + crw * tz + (crx * ty - cry * tx);
	};
	float rx, ry, rz, ux, uy, uz, fx, fy, fz;
	qr(1, 0, 0, rx, ry, rz);
	qr(0, 1, 0, ux, uy, uz);
	qr(0, 0, -1, fx, fy, fz);
	const float cpx = cam.Position[0];
	const float cpy = cam.Position[1];
	const float cpz = cam.Position[2];

	// View & projection (column-major, OpenGL convention — same as gizmo)
	// Column-major: element [col*4 + row]
	const float V[16] = {
		rx, ux, -fx, 0.0f,
		ry, uy, -fy, 0.0f,
		rz, uz, -fz, 0.0f,
		-(rx * cpx + ry * cpy + rz * cpz),
		-(ux * cpx + uy * cpy + uz * cpz),
		(fx * cpx + fy * cpy + fz * cpz),
		1.0f
	};
	const float aspect = ViewportPanelSize.x / ViewportPanelSize.y;
	const float fovRad = cam.FoVDeg * 3.14159265f / 180.0f;
	const float F      = 1.0f / std::tan(fovRad * 0.5f);
	const float zNear = 0.1f, zFar = 5000.0f, dz = zNear - zFar;
	const float P[16] = {
		F / aspect, 0.0f, 0.0f, 0.0f,
		0.0f, F, 0.0f, 0.0f,
		0.0f, 0.0f, zFar / dz, -1.0f,
		0.0f, 0.0f, (zFar * zNear) / dz, 0.0f
	};

	// Transform a world-space Y=0 point (wx, wz) → view space → clip space.
	// Returns clip-space (cx, cy, cw). cw = -vz; positive means in front.
	auto toClip = [&](float wx, float wz, float& cx, float& cy, float& cw)
	{
		float vx = V[0] * wx + V[8] * wz + V[12]; // V[4]*0 dropped (wy=0)
		float vy = V[1] * wx + V[9] * wz + V[13];
		float vz = V[2] * wx + V[10] * wz + V[14];
		cx       = P[0] * vx;
		cy       = P[5] * vy;
		cw       = -vz; // projection[11] = -1
	};

	// Convert clip-space (cx, cy, cw) → ImGui screen position.
	auto toScreen = [&](float cx, float cy, float cw) -> ImVec2
	{
		float ndcX = cx / cw;
		float ndcY = cy / cw;
		return {
			(ndcX + 1.0f) * 0.5f * ViewportPanelSize.x + ViewportPanelPos.x,
			(1.0f - (ndcY + 1.0f) * 0.5f) * ViewportPanelSize.y + ViewportPanelPos.y
		};
	};

	// Draw a Y=0 line from (wx0,wz0) to (wx1,wz1) with near-plane clipping.
	const float kNear = 0.05f;
	ImDrawList* dl    = ImGui::GetWindowDrawList();

	auto drawLine = [&](float wx0, float wz0, float wx1, float wz1, ImU32 col, float thickness)
	{
		float cx0, cy0, cw0, cx1, cy1, cw1;
		toClip(wx0, wz0, cx0, cy0, cw0);
		toClip(wx1, wz1, cx1, cy1, cw1);
		if (cw0 <= kNear && cw1 <= kNear) return; // both behind

		// Clip near-plane on either endpoint
		if (cw0 <= kNear)
		{
			float t = (kNear - cw0) / (cw1 - cw0);
			cx0     = cx0 + t * (cx1 - cx0);
			cy0     = cy0 + t * (cy1 - cy0);
			cw0     = kNear;
		}
		else if (cw1 <= kNear)
		{
			float t = (kNear - cw1) / (cw0 - cw1);
			cx1     = cx1 + t * (cx0 - cx1);
			cy1     = cy1 + t * (cy0 - cy1);
			cw1     = kNear;
		}
		dl->AddLine(toScreen(cx0, cy0, cw0), toScreen(cx1, cy1, cw1), col, thickness);
	};

	constexpr int kHalf   = 50; // grid extends ±50 units from world origin
	constexpr float kStep = 1.0f;
	const float ext       = kHalf * kStep;

	const ImU32 kLine  = IM_COL32(70, 70, 90, 130);
	const ImU32 kAxisX = IM_COL32(180, 60, 60, 200); // X-axis red
	const ImU32 kAxisZ = IM_COL32(60, 60, 180, 200); // Z-axis blue

	// Lines parallel to X (varying Z)
	for (int i = -kHalf; i <= kHalf; ++i)
	{
		float z   = i * kStep;
		ImU32 col = (i == 0) ? kAxisX : kLine;
		float th  = (i == 0) ? 1.5f : 1.0f;
		drawLine(-ext, z, ext, z, col, th);
	}
	// Lines parallel to Z (varying X)
	for (int i = -kHalf; i <= kHalf; ++i)
	{
		float x   = i * kStep;
		ImU32 col = (i == 0) ? kAxisZ : kLine;
		float th  = (i == 0) ? 1.5f : 1.0f;
		drawLine(x, -ext, x, ext, col, th);
	}
}

void EditorContext::DrawViewportPanel(const char* title, WorldViewport& vp)
{
	if (WorkspaceMainNode != 0)
		ImGui::SetNextWindowDockID(WorkspaceMainNode, ImGuiCond_Appearing);
	else
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
