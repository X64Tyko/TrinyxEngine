#pragma once

class Registry;
struct EngineConfig;

/// EditorContext — owns all editor UI state and panel drawing.
///
/// Called by the renderer between ImGui::NewFrame() and ImGui::Render().
/// All ImGui:: calls for editor panels live here, keeping editor logic
/// separated from engine rendering code.
class EditorContext
{
public:
	EditorContext()  = default;
	~EditorContext() = default;

	void Initialize(Registry* registry, const EngineConfig* config);

	/// Build the editor UI for this frame.  Called on the render thread
	/// after ImGui::NewFrame(), before ImGui::Render().
	void BuildFrame();

private:
	Registry* RegistryPtr         = nullptr;
	const EngineConfig* ConfigPtr = nullptr;

	bool bShowDemoWindow = true;
};
