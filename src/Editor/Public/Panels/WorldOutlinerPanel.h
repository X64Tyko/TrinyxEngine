#pragma once
#include "EditorPanel.h"

class WorldOutlinerPanel : public EditorPanel
{
public:
	WorldOutlinerPanel()
		: EditorPanel("World Outliner")
	{
	}

	void Draw(EditorState& state) override;
};