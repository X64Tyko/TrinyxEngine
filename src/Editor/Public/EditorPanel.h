#pragma once
#include "imgui.h"

struct EditorState;

/// Virtual base class for all editor panels.
/// OOP is explicitly allowed in editor code — the ECS anti-virtual rules
/// apply to engine components, not editor UI.
class EditorPanel
{
public:
	explicit EditorPanel(const char* title)
		: Title(title)
	{
	}

	virtual ~EditorPanel() = default;

	void Tick(EditorState& state)
	{
		if (bForceMainViewport)
		{
			ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
			bForceMainViewport = false;
		}
		if (bVisible) Draw(state);
	}

	virtual void Draw(EditorState& state) = 0;

	/// Begin() with horizontal WindowPadding=8 so content doesn't touch panel edges.
	/// PanelHeader draws edge-to-edge by using GetWindowPos() directly — padding
	/// only affects the content cursor below the header bar.
	bool BeginPadded(ImGuiWindowFlags flags = 0)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 0.0f));
		bool open = ImGui::Begin(Title, &bVisible, flags);
		ImGui::PopStyleVar();
		return open;
	}

	const char* Title;
	bool bVisible           = true;
	bool bForceMainViewport = false;
};
