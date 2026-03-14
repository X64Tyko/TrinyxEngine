#include "EditorContext.h"
#include "imgui.h"
#include "Logger.h"

void EditorContext::Initialize(Registry* registry, const EngineConfig* config)
{
	LOG_DEBUG("We have an editor... sorta");
	RegistryPtr = registry;
	ConfigPtr   = config;
}

void EditorContext::BuildFrame()
{
	if (bShowDemoWindow)
	{
		ImGui::ShowDemoWindow(&bShowDemoWindow);
	}
}
