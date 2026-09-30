#pragma once
#if !defined(TNX_ENABLE_EDITOR)
#error "EditorState.h requires TNX_ENABLE_EDITOR"
#endif

#include "Types.h"
#include <string>

class Archetype;
class AssetDatabase;
class EditorContext;
class Registry;
class LogicThreadBase;
class ReplicationSystem;
class TrinyxEngine;
struct Chunk;
struct EngineConfig;

/// Transient editor-wide state shared across all panels.
/// Lives on the render thread — no synchronization needed between panels.
struct EditorState
{
	// --- Engine pointers (set once at init, never null after that) ---
	Registry* RegistryPtr         = nullptr;
	const EngineConfig* ConfigPtr = nullptr;
	LogicThreadBase* LogicPtr     = nullptr;

	// --- Selection ---
	enum class SelectionType : uint8_t
	{
		None,
		Construct,
		Archetype,
		Entity
	};

	SelectionType Selection = SelectionType::None;

	// Construct selection (valid when SelectionType::Construct)
	uint32_t SelectedConstructID          = 0;
	void* SelectedConstructPtr            = nullptr;
	const char* SelectedConstructTypeName = nullptr;

	// Archetype selection (valid for both Archetype and Entity modes)
	ClassID SelectedClassID      = 0;
	Archetype* SelectedArchetype = nullptr;

	// Entity selection (only valid when SelectionType::Entity)
	Chunk* SelectedChunk        = nullptr;
	uint16_t SelectedLocalIndex = 0;
	uint32_t SelectedCacheIndex = 0;

	void ClearSelection()
	{
		Selection                 = SelectionType::None;
		SelectedConstructID       = 0;
		SelectedConstructPtr      = nullptr;
		SelectedConstructTypeName = nullptr;
		SelectedClassID           = 0;
		SelectedArchetype         = nullptr;
		SelectedChunk             = nullptr;
		SelectedLocalIndex        = 0;
		SelectedCacheIndex        = 0;
	}

	// --- Gizmo ---
	enum class GizmoOp : uint8_t
	{
		Translate,
		Rotate,
		Scale
	};

	GizmoOp CurrentGizmoOp   = GizmoOp::Translate;
	bool bGizmoWorldMode     = true; // true = WORLD, false = LOCAL
	bool bGizmoSnap          = false;
	float GizmoSnapTranslate = 0.5f;
	float GizmoSnapRotate    = 15.0f;
	float GizmoSnapScale     = 0.25f;

	// --- Scene file ---
	std::string CurrentScenePath; // Full path to the loaded .tnxscene file (empty = untitled)
	std::string CurrentSceneName = "Untitled";
	bool bSceneDirty             = false; // True if scene has been modified since last save

	// --- Scene flow metadata (saved in .tnxscene, used by PIE) ---
	std::string SceneDefaultState; // Name of the FlowState to load on PIE start
	std::string SceneDefaultMode;  // Name of the GameMode to activate on PIE start

	// --- Engine access (for Spawn handshake, scene load, etc.) ---
	TrinyxEngine* EnginePtr = nullptr;

	// --- Networking (null when PIE is not active) ---
	ReplicationSystem* ReplicatorPtr = nullptr;

	// --- Asset database (editor only, set by EditorContext) ---
	AssetDatabase* AssetDB   = nullptr;
	EditorContext* EditorCtx = nullptr;

#ifdef TNX_DEBUG_RENDERING
	// Active debug draw mode — written by DebuggerPanel, consumed by FillGpuFrameDataForViewport.
	uint8_t DebugDrawMode = 0;
#endif
};
