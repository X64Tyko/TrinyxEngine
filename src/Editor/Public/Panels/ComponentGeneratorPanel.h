#pragma once
#if !defined(TNX_ENABLE_EDITOR)
#error "ComponentGeneratorPanel.h requires TNX_ENABLE_EDITOR"
#endif

#include "EditorPanel.h"
#include <string>
#include <vector>

/// ComponentGeneratorPanel — form-based editor tool for generating Trinyx ECS component headers.
///
/// Authors define a component name, storage tier, system group, and a list of typed fields.
/// The panel previews the generated C++ header and can export it directly to disk.
class ComponentGeneratorPanel : public EditorPanel
{
public:
	ComponentGeneratorPanel()
		: EditorPanel("Component Generator")
	{
	}

	void Draw(EditorState& state) override;

private:
	struct FieldDef
	{
		char Name[64]  = {};
		int TypeIndex  = 0; // index into kFieldTypeNames[]
	};

	char CompName[64]    = "CMyComponent";
	int TierIndex        = 0; // 0=Temporal, 1=Volatile, 2=Cold
	int GroupIndex       = 0; // 0=Render, 1=Physics, 2=Logic, 3=Dual
	std::vector<FieldDef> Fields;

	std::string GeneratedCode;
	char OutputPath[512]   = {};
	char StatusMessage[256] = {};
	bool bStatusError      = false;

	/// Rebuild GeneratedCode from current form state.
	void RegenerateCode();

	/// Write GeneratedCode to OutputPath.
	bool ExportToFile();
};
