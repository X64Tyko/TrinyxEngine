#pragma once
#if !defined(TNX_ENABLE_EDITOR)
#error "EditorContext.h requires TNX_ENABLE_EDITOR"
#endif

#include <memory>
#include <string>
#include <vector>

#include "AssetDatabase.h"
#include "EditorState.h"

class EditorPanel;
class LogicThread;
class TrinyxEngine;

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

	void Initialize(TrinyxEngine* engine, LogicThread* logic);

	/// Build the editor UI for this frame.  Called on the render thread
	/// after ImGui::NewFrame(), before ImGui::Render().
	void BuildFrame();

	/// Register a panel. EditorContext takes ownership.
	template <typename T, typename... Args>
	T* AddPanel(Args&&... args)
	{
		auto panel = std::make_unique<T>(std::forward<Args>(args)...);
		T* ptr     = panel.get();
		Panels.push_back(std::move(panel));
		return ptr;
	}

private:
	void BuildDockspace();
	void BuildMenuBar();
	void ApplyDefaultLayout(unsigned int dockspaceID);

	TrinyxEngine* EnginePtr = nullptr;
	LogicThread* LogicPtr   = nullptr;

	EditorState State;
	AssetDatabase AssetDB;
	std::vector<std::unique_ptr<EditorPanel>> Panels;

	void DrawFileDialog();
	void DrawUnsavedWarning();

	enum class PendingActionType : uint8_t { None, OpenScene };

	bool bShowDemoWindow            = false;
	bool bShowMetrics               = false;
	bool bFirstFrame                = true;
	bool bShowFileDialog            = false;
	bool bFileDialogForSave         = false;
	bool bShowUnsavedWarning        = false;
	PendingActionType PendingAction = PendingActionType::None;
	std::string FileDialogPath;
};
