#pragma once

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
		if (bVisible) Draw(state);
	}

	virtual void Draw(EditorState& state) = 0;

	const char* Title;
	bool bVisible = true;
};