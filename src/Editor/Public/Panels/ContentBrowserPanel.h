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

private:
	// Filter state
	int TypeFilter = 0; // 0 = All, maps to AssetType values
};