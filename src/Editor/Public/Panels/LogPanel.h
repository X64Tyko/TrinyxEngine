#pragma once
#include "EditorPanel.h"

class LogPanel : public EditorPanel
{
public:
	LogPanel()
		: EditorPanel("Log")
	{
	}

	void Draw(EditorState& state) override;
};