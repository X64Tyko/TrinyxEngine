#pragma once
#include "EditorPanel.h"
#include <cstddef>
#include <cstdint>

class DetailsPanel : public EditorPanel
{
public:
	DetailsPanel()
		: EditorPanel("Details")
	{
	}

	void Draw(EditorState& state) override;

private:
	// Returns true if the field was edited. Writes directly to fieldArray at entityIndex.
	bool EditFieldValue(const char* label, size_t fieldSize, void* fieldArray,
						uint32_t entityIndex, uint8_t valueType);
};