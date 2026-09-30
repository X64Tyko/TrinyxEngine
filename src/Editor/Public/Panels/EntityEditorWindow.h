#pragma once
#if !defined(TNX_ENABLE_EDITOR)
#error "EntityEditorWindow.h requires TNX_ENABLE_EDITOR"
#endif

#include "EditorPanel.h"
#include "NodeGraphCanvas.h"
#include <string>
#include <vector>

/// One open Entity type in the editor — component list + spawn-init graph.
struct EntityDoc
{
	std::string TypeName;
	std::string FilePath;

	struct ComponentEntry
	{
		char TypeName[64] = "CTransform";
		bool bSelected    = false;
	};

	std::vector<ComponentEntry> Components;

	/// Spawn initializer graph — sets default field values.
	NodeGraphCanvas SpawnGraph;

	char OutputPath[512] = {};
	char StatusMsg[256]  = {};
	bool bStatusError    = false;

	explicit EntityDoc(const char* typeName, const char* filePath = nullptr);
};

/// Dockable editor window for Entity types.
/// Tab bar across the top — one tab per open EntityDoc.
/// Per-tab layout: Component panel (left) | Spawn-init graph (centre) | Info (right).
class EntityEditorWindow : public EditorPanel
{
public:
	EntityEditorWindow();
	~EntityEditorWindow() override = default;

	void Draw(EditorState& state) override;

	/// Open (or focus) an Entity type by name. Parses filePath when provided.
	void OpenEntity(const char* typeName, const char* filePath = nullptr);

private:
	std::vector<EntityDoc> Docs;
	int ActiveTab = 0;

	char NewTypeBuf[64] = "EMyEntity";
	bool bShowNewDialog = false;

	bool bLeftPaneVisible  = true;
	bool bRightPaneVisible = true;

	void DrawTabBar();
	void DrawComponentPanel(EntityDoc& doc, float height);
	void DrawSpawnGraph(EntityDoc& doc, float width, float height);
	void DrawInfoPanel(EntityDoc& doc, float height, EditorState& state);
};
