#pragma once
#if !defined(TNX_ENABLE_EDITOR)
#error "PrefabEditorWindow.h requires TNX_ENABLE_EDITOR"
#endif

#include "EditorPanel.h"
#include "EngineConfig.h"
#include "FieldMeta.h"
#include "Json.h"
#include <memory>
#include <string>
#include <vector>

class FlowManagerBase;
struct WorldViewport;

/// Metadata for one open .tnxprefab file.
struct PrefabDoc
{
	std::string FilePath;
	std::string PrefabName;
	std::string PrefabType;   // "Entity" or "Construct"
	std::string BaseTypeName; // e.g. "EPlayer"

	JsonValue RootJson;
	bool bDirty                 = false;
	bool bTabOpen               = true;
	bool bPreviewRebuildPending = false;

	// Preview world (Entity-type prefabs only — Construct preview not supported)
	std::unique_ptr<FlowManagerBase> PreviewFlow;
	std::unique_ptr<WorldViewport> PreviewViewport;
	EngineConfig PreviewConfig;
	bool bPreviewReady = false;
	std::string PreviewError; // Set when preview fails; shown in the viewport area.

	// Orbit camera state (render-thread owned)
	float OrbitYaw   = 0.0f;
	float OrbitPitch = 0.35f; // ~20° above equator
	float OrbitDist  = 5.0f;
};

/// Dockable prefab editor window. Tab bar for multiple open prefabs.
/// Per-tab layout: Inspector (left) | 3D preview viewport (right).
class PrefabEditorWindow : public EditorPanel
{
public:
	PrefabEditorWindow();
	~PrefabEditorWindow() override = default;

	void Draw(EditorState& state) override;

	/// Open (or focus) a prefab file. Render-thread safe.
	void OpenPrefab(const std::string& filePath, EditorState& state);

	/// Release all preview worlds before editor shutdown.
	void DestroyAllPreviews(EditorState& state);

private:
	std::vector<PrefabDoc> Docs;
	int ActiveTab = 0;

	void DrawTabBar(EditorState& state);
	void DrawInspector(PrefabDoc& doc, float width, EditorState& state);
	void DrawPreviewViewport(PrefabDoc& doc, float availW, float availH, EditorState& state);

	void ParseFromJson(PrefabDoc& doc);
	void RebuildPreviewWorld(PrefabDoc& doc, EditorState& state);
	void DestroyPreviewWorld(PrefabDoc& doc, EditorState& state);
	void UpdateOrbitCamera(PrefabDoc& doc);
	void SaveDoc(PrefabDoc& doc);

	// After any push_back or erase on Docs, rewire each PreviewFlow's Config pointer
	// to the vector-resident PrefabDoc::PreviewConfig (addresses shift on reallocation).
	void RewireAllPreviewConfigs();

	/// Draw one editable field row inside the inspector.
	void DrawFieldRow(PrefabDoc& doc, const std::string& compName,
		const std::string& fieldName, FieldValueType valueType,
		AssetType refAssetType, EditorState& state);
};
