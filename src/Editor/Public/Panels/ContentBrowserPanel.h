#pragma once
#include "EditorPanel.h"

class ContentBrowserPanel : public EditorPanel
{
public:
	ContentBrowserPanel()
		: EditorPanel("Content Browser")
	{
	}

	void Draw(EditorState& state) override;
};