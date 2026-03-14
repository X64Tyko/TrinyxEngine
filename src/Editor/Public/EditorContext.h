#pragma once
#include <memory>
#include <vector>

#include "EditorState.h"

class EditorPanel;
class Registry;
class LogicThread;
struct EngineConfig;

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

	void Initialize(Registry* registry, const EngineConfig* config, LogicThread* logic);

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

	Registry* RegistryPtr         = nullptr;
	const EngineConfig* ConfigPtr = nullptr;
	LogicThread* LogicPtr         = nullptr;

	EditorState State;
	std::vector<std::unique_ptr<EditorPanel>> Panels;

	bool bShowDemoWindow = false;
	bool bShowMetrics    = false;
	bool bFirstFrame     = true;
};
