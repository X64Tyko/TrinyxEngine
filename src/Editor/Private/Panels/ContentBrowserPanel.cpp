#include "Panels/ContentBrowserPanel.h"
#include "EditorState.h"
#include "imgui.h"

void ContentBrowserPanel::Draw(EditorState& /*state*/)
{
	ImGui::Begin(Title, &bVisible);
	ImGui::TextDisabled("(Phase 6)");
	ImGui::End();
}