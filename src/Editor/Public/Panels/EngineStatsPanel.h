#pragma once
#include "EditorPanel.h"

class EngineStatsPanel : public EditorPanel
{
public:
	EngineStatsPanel()
		: EditorPanel("Engine Stats")
	{
	}

	void Draw(EditorState& state) override;
};