#pragma once
#if !defined(TNX_ENABLE_EDITOR)
#error "NodeScriptPanel.h requires TNX_ENABLE_EDITOR"
#endif

#include "EditorPanel.h"
#include "NodeGraphCanvas.h"
#include <string>

/// Standalone node-script panel — wraps a NodeGraphCanvas and adds the
/// entity-name toolbar, code generation, and file export UI.
class NodeScriptPanel : public EditorPanel
{
public:
	NodeScriptPanel();
	~NodeScriptPanel() override = default;

	void Draw(EditorState& state) override;

private:
	NodeGraphCanvas Canvas;

	char TargetEntityName[64] = "EMyEntity";
	char OutputPath[512]      = {};
	std::string GeneratedCode;
	bool bShowCodePreview = false;
	char StatusMsg[1024]  = {};
	bool bStatusError     = false;

	void GenerateCode();
	bool ExportToFile();
};
