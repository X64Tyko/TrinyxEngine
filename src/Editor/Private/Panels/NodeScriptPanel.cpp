#include "Panels/NodeScriptPanel.h"
#if !defined(TNX_ENABLE_EDITOR)
#error "NodeScriptPanel.cpp requires TNX_ENABLE_EDITOR"
#endif

#include "EditorState.h"
#include "EngineConfig.h"
#include "Logger.h"
#include "TnxStyle.h"
#include "TnxWidgets.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>

NodeScriptPanel::NodeScriptPanel()
	: EditorPanel("Node Script")
{
}

// ---------------------------------------------------------------------------
// Code generation
// ---------------------------------------------------------------------------

void NodeScriptPanel::GenerateCode()
{
	using NK = NodeGraphCanvas::NodeKind;
	using PT = NodeGraphCanvas::PinType;
	using PD = NodeGraphCanvas::PinDir;

	GeneratedCode.clear();
	StatusMsg[0] = '\0';
	bStatusError = false;

	bool hasBranch = false, hasPrePostPhysics = false;
	for (const auto& n : Canvas.Nodes)
	{
		if (n.Kind == NK::Branch) hasBranch = true;
		if (n.Kind == NK::Event_OnPrePhysics || n.Kind == NK::Event_OnPostPhysics)
			hasPrePostPhysics = true;
	}
	if (hasBranch && hasPrePostPhysics)
	{
		snprintf(StatusMsg, sizeof(StatusMsg),
			"ERROR: Branch nodes are forbidden in pre/post-physics graphs (determinism constraint).");
		bStatusError  = true;
		GeneratedCode = "// Code generation blocked: Branch is not permitted in pre/post-physics.\n"
						"// Remove Branch nodes or move them to an OnUpdate graph.\n";
		return;
	}

	auto varName = [](int nodeID, int pinID) -> std::string
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "_n%d_p%d", nodeID, pinID);
		return buf;
	};

	std::unordered_map<int, std::pair<int, int>> pinSrc;
	for (const auto& link : Canvas.Links)
		pinSrc[link.DstPinID] = { link.SrcNodeID, link.SrcPinID };

	std::function<std::string(int, int)> pinExpr;
	pinExpr = [&](int nodeID, int pinID) -> std::string
	{
		auto it = pinSrc.find(pinID);
		if (it == pinSrc.end())
		{
			const NodeGraphCanvas::Pin* p = Canvas.FindPin(nodeID, pinID);
			if (!p) return "0.0f";
			switch (p->Type)
			{
				case PT::Vec3: return "{0.0f, 0.0f, 0.0f}";
				case PT::Bool: return "false";
				case PT::Int: return "0";
				default: return "0.0f";
			}
		}
		int srcNode                           = it->second.first;
		int srcPin                            = it->second.second;
		const NodeGraphCanvas::ScriptNode* sn = Canvas.FindNode(srcNode);
		if (!sn) return "0.0f";

		switch (sn->Kind)
		{
			case NK::Math_Add:
			case NK::Math_Subtract:
			case NK::Math_Multiply:
			case NK::Math_Divide:
			{
				int pinA = -1, pinB = -1;
				for (const auto& p : sn->Pins)
					if (p.Dir == PD::Input)
					{
						if (pinA == -1)
							pinA = p.ID;
						else
							pinB = p.ID;
					}
				const char* op = (sn->Kind == NK::Math_Add ? "+" : sn->Kind == NK::Math_Subtract ? "-"
															   : sn->Kind == NK::Math_Multiply   ? "*"
																								 : "/");
				return "(" + pinExpr(srcNode, pinA) + " " + op + " " + pinExpr(srcNode, pinB) + ")";
			}
			case NK::Math_Clamp:
			{
				int pVal = -1, pMin = -1, pMax = -1;
				for (const auto& p : sn->Pins)
					if (p.Dir == PD::Input)
					{
						if (pVal == -1)
							pVal = p.ID;
						else if (pMin == -1)
							pMin = p.ID;
						else
							pMax = p.ID;
					}
				return "std::clamp(" + pinExpr(srcNode, pVal) + ", " + pinExpr(srcNode, pMin) + ", " + pinExpr(srcNode, pMax) + ")";
			}
			case NK::Math_Lerp:
			{
				int pA = -1, pB = -1, pAlpha = -1;
				for (const auto& p : sn->Pins)
					if (p.Dir == PD::Input)
					{
						if (pA == -1)
							pA = p.ID;
						else if (pB == -1)
							pB = p.ID;
						else
							pAlpha = p.ID;
					}
				return "(" + pinExpr(srcNode, pA) + " + (" + pinExpr(srcNode, pB) + " - " + pinExpr(srcNode, pA) + ") * " + pinExpr(srcNode, pAlpha) + ")";
			}
			case NK::Vec3_Length:
			{
				int pIn = -1;
				for (const auto& p : sn->Pins)
					if (p.Dir == PD::Input)
					{
						pIn = p.ID;
						break;
					}
				return "glm::length(" + pinExpr(srcNode, pIn) + ")";
			}
			case NK::Vec3_Normalize:
			{
				int pIn = -1;
				for (const auto& p : sn->Pins)
					if (p.Dir == PD::Input)
					{
						pIn = p.ID;
						break;
					}
				return "glm::normalize(" + pinExpr(srcNode, pIn) + ")";
			}
			case NK::Vec3_Scale:
			{
				int pIn = -1, pScale = -1;
				for (const auto& p : sn->Pins)
					if (p.Dir == PD::Input)
					{
						if (pIn == -1)
							pIn = p.ID;
						else
							pScale = p.ID;
					}
				return "(" + pinExpr(srcNode, pIn) + " * " + pinExpr(srcNode, pScale) + ")";
			}
			case NK::Vec3_Make:
			{
				int pX = -1, pY = -1, pZ = -1;
				for (const auto& p : sn->Pins)
					if (p.Dir == PD::Input)
					{
						if (pX == -1)
							pX = p.ID;
						else if (pY == -1)
							pY = p.ID;
						else
							pZ = p.ID;
					}
				return "glm::vec3(" + pinExpr(srcNode, pX) + ", " + pinExpr(srcNode, pY) + ", " + pinExpr(srcNode, pZ) + ")";
			}
			case NK::GetPosition: return "glm::vec3(Transform.PosX, Transform.PosY, Transform.PosZ)";
			case NK::GetVelocity: return "glm::vec3(Velocity.X, Velocity.Y, Velocity.Z)";
			default: return varName(srcNode, srcPin);
		}
	};

	std::function<std::string(int, int, int)> genExec;
	genExec = [&](int srcNodeID, int srcExecPinID, int indent) -> std::string
	{
		const NodeGraphCanvas::NodeLink* foundLink = nullptr;
		for (const auto& link : Canvas.Links)
			if (link.SrcNodeID == srcNodeID && link.SrcPinID == srcExecPinID)
			{
				foundLink = &link;
				break;
			}
		if (!foundLink) return "";

		const NodeGraphCanvas::ScriptNode* dst = Canvas.FindNode(foundLink->DstNodeID);
		if (!dst) return "";

		std::string pad(static_cast<size_t>(indent) * 4, ' ');
		std::string body;

		int nextExecSrc = -1;
		for (const auto& p : dst->Pins)
			if (p.Dir == PD::Output && p.Type == PT::Exec)
			{
				nextExecSrc = p.ID;
				break;
			}

		switch (dst->Kind)
		{
			case NK::SetPosition:
			{
				int pVal = -1;
				for (const auto& p : dst->Pins)
					if (p.Dir == PD::Input && p.Type == PT::Vec3)
					{
						pVal = p.ID;
						break;
					}
				body += pad + "{ auto _pos = " + pinExpr(dst->ID, pVal) + ";\n";
				body += pad + "  Transform.PosX = _pos.x; Transform.PosY = _pos.y; Transform.PosZ = _pos.z; }\n";
				break;
			}
			case NK::SetVelocity:
			{
				int pVal = -1;
				for (const auto& p : dst->Pins)
					if (p.Dir == PD::Input && p.Type == PT::Vec3)
					{
						pVal = p.ID;
						break;
					}
				body += pad + "{ auto _vel = " + pinExpr(dst->ID, pVal) + ";\n";
				body += pad + "  Velocity.X = _vel.x; Velocity.Y = _vel.y; Velocity.Z = _vel.z; }\n";
				break;
			}
			case NK::ApplyImpulse:
			{
				int pImp = -1;
				for (const auto& p : dst->Pins)
					if (p.Dir == PD::Input && p.Type == PT::Vec3)
					{
						pImp = p.ID;
						break;
					}
				body += pad + "GetPhysics()->ApplyImpulse(BodyID, " + pinExpr(dst->ID, pImp) + ");\n";
				break;
			}
			case NK::SetProperty:
			{
				int pVal = -1;
				for (const auto& p : dst->Pins)
					if (p.Dir == PD::Input && p.Type != PT::Exec)
					{
						pVal = p.ID;
						break;
					}
				body += pad + varName(dst->ID, 0) + " = " + pinExpr(dst->ID, pVal) + ";\n";
				break;
			}
			case NK::Sequence:
			{
				for (const auto& p : dst->Pins)
					if (p.Dir == PD::Output && p.Type == PT::Exec) body += genExec(dst->ID, p.ID, indent);
				break;
			}
			default: break;
		}

		if (nextExecSrc != -1) body += genExec(dst->ID, nextExecSrc, indent);
		return body;
	};

	std::string out;
	out += "#pragma once\n// Auto-generated by Trinyx Node Script — do not edit by hand.\n\n";
	out += "#include \"Construct.h\"\n#include \"ConstructView.h\"\n\n";
	out += std::string("#include \"") + TargetEntityName + ".h\"\n\n";
	out += std::string("class ") + TargetEntityName + "Script : public Construct<" + TargetEntityName + "Script>\n{\n";
	out += std::string("    ConstructView<") + TargetEntityName + "> Body;\n\npublic:\n";
	out += "    void InitializeViews() { Body.Initialize(this); }\n\n";

	for (const auto& event : Canvas.Nodes)
	{
		if (!event.IsEventNode()) continue;
		const char* methodName = nullptr;
		const char* sig        = nullptr;
		switch (event.Kind)
		{
			case NK::Event_OnPrePhysics:
				methodName = "PrePhysics";
				sig        = "SimFloat dt";
				break;
			case NK::Event_OnPostPhysics:
				methodName = "PostPhysics";
				sig        = "SimFloat dt";
				break;
			case NK::Event_OnUpdate:
				methodName = "ScalarUpdate";
				sig        = "float dt";
				break;
			case NK::Event_OnSpawn:
				methodName = "OnSpawn";
				sig        = "";
				break;
			case NK::Event_OnDestroy:
				methodName = "OnDestroy";
				sig        = "";
				break;
			default: continue;
		}
		out += std::string("    void ") + methodName + "(" + sig + ")\n    {\n";
		for (const auto& p : event.Pins)
			if (p.Dir == PD::Output && p.Type == PT::Exec)
			{
				out += genExec(event.ID, p.ID, 2);
				break;
			}
		out += "    }\n\n";
	}

	out += "};\n";
	GeneratedCode = std::move(out);
}

bool NodeScriptPanel::ExportToFile()
{
	if (OutputPath[0] == '\0')
	{
		snprintf(StatusMsg, sizeof(StatusMsg), "Set an output path before exporting.");
		bStatusError = true;
		return false;
	}
	try
	{
		std::filesystem::create_directories(std::filesystem::path(OutputPath).parent_path());
	}
	catch (const std::exception& ex)
	{
		snprintf(StatusMsg, sizeof(StatusMsg), "Failed to create output directory: %s", ex.what());
		bStatusError = true;
		LOG_ENG_ERROR_F("[NodeScript] Failed to create output directory: %s", ex.what());
		return false;
	}
	std::ofstream file(OutputPath);
	if (!file.is_open())
	{
		snprintf(StatusMsg, sizeof(StatusMsg), "Failed to open: %s", OutputPath);
		bStatusError = true;
		return false;
	}
	file << GeneratedCode;
	const std::string s = std::string("Exported: ") + OutputPath;
	strncpy(StatusMsg, s.c_str(), sizeof(StatusMsg) - 1);
	StatusMsg[sizeof(StatusMsg) - 1] = '\0';
	LOG_ENG_INFO(s.c_str());
	bStatusError = false;
	return true;
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

void NodeScriptPanel::Draw(EditorState& state)
{
	if (!BeginPadded(ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		ImGui::End();
		return;
	}
	TnxWidgets::PanelHeader(nullptr, "Node Script");

	if (OutputPath[0] == '\0' && state.ConfigPtr && state.ConfigPtr->ProjectDir[0] != '\0')
	{
		std::string suggested = std::string(state.ConfigPtr->ProjectDir)
								+ "/src/Scripted/" + TargetEntityName + "Script.h";
		snprintf(OutputPath, sizeof(OutputPath), "%s", suggested.c_str());
	}

	ImGui::SetNextItemWidth(160.0f);
	if (ImGui::InputText("Entity##TargetName", TargetEntityName, sizeof(TargetEntityName)))
	{
		if (state.ConfigPtr && state.ConfigPtr->ProjectDir[0] != '\0')
		{
			std::string suggested = std::string(state.ConfigPtr->ProjectDir)
									+ "/src/Scripted/" + TargetEntityName + "Script.h";
			snprintf(OutputPath, sizeof(OutputPath), "%s", suggested.c_str());
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Generate Code"))
	{
		GenerateCode();
		bShowCodePreview = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Export"))
	{
		GenerateCode();
		ExportToFile();
	}
	ImGui::SameLine();
	if (ImGui::Button(bShowCodePreview ? "Hide Code" : "Show Code"))
		bShowCodePreview = !bShowCodePreview;

	if (StatusMsg[0] != '\0')
	{
		ImGui::SameLine();
		ImVec4 col = bStatusError ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f) : ImVec4(0.35f, 1.0f, 0.35f, 1.0f);
		ImGui::TextColored(col, "%s", StatusMsg);
	}

	ImGui::Separator();

	float totalW   = ImGui::GetContentRegionAvail().x;
	float paletteW = 160.0f;
	float codeW    = bShowCodePreview ? 280.0f : 0.0f;
	float canvasW  = totalW - paletteW - codeW - (bShowCodePreview ? 8.0f : 4.0f);
	float canvasH  = ImGui::GetContentRegionAvail().y;

	Canvas.DrawPalette("nsp", paletteW, canvasH);
	ImGui::SameLine();
	Canvas.DrawCanvas("nsp", canvasW, canvasH);

	if (bShowCodePreview)
	{
		ImGui::SameLine();
		ImGui::BeginChild("##CodePane", ImVec2(codeW, canvasH), ImGuiChildFlags_None,
			ImGuiWindowFlags_HorizontalScrollbar);
		ImGui::Text("Generated C++");
		ImGui::Separator();
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputText("##OutPathNS", OutputPath, sizeof(OutputPath));
		ImGui::BeginChild("##CodeText", ImVec2(0, 0), ImGuiChildFlags_None,
			ImGuiWindowFlags_HorizontalScrollbar);
		if (!GeneratedCode.empty())
			ImGui::TextUnformatted(GeneratedCode.c_str());
		else
			ImGui::TextDisabled("Click 'Generate Code' to preview.");
		ImGui::EndChild();
		ImGui::EndChild();
	}

	ImGui::End();
}
