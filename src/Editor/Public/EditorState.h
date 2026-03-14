#pragma once
#include "Types.h"

class Archetype;
struct Chunk;

/// Transient editor-wide state shared across all panels.
/// Lives on the render thread — no synchronization needed between panels.
struct EditorState
{
	// --- Selection ---
	enum class SelectionType : uint8_t { None, Archetype, Entity };

	SelectionType Selection = SelectionType::None;

	// Archetype selection (valid for both Archetype and Entity modes)
	ClassID SelectedClassID      = 0;
	Archetype* SelectedArchetype = nullptr;

	// Entity selection (only valid when SelectionType::Entity)
	Chunk* SelectedChunk         = nullptr;
	uint16_t SelectedLocalIndex  = 0;
	uint32_t SelectedGlobalIndex = 0;

	void ClearSelection()
	{
		Selection           = SelectionType::None;
		SelectedClassID     = 0;
		SelectedArchetype   = nullptr;
		SelectedChunk       = nullptr;
		SelectedLocalIndex  = 0;
		SelectedGlobalIndex = 0;
	}
};