#pragma once
#include "EditorPanel.h"

class DetailsPanel : public EditorPanel
{
public:
	DetailsPanel()
		: EditorPanel("Details")
	{
	}

	void Draw(EditorState& state) override;
};