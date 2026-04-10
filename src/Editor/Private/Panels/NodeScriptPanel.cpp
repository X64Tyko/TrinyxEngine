#include "Panels/NodeScriptPanel.h"
#if !defined(TNX_ENABLE_EDITOR)
#error "NodeScriptPanel.cpp requires TNX_ENABLE_EDITOR"
#endif

#include "EditorState.h"
#include "EngineConfig.h"
#include "Logger.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr float kNodeWidth      = 180.0f;
static constexpr float kNodeTitleH     = 22.0f;
static constexpr float kPinRowH        = 18.0f;
static constexpr float kPinRadius      = 5.0f;
static constexpr float kPinHitRadius   = 8.0f;
static constexpr float kGridStep       = 48.0f;
static constexpr float kBezierStrength = 80.0f;

// Node title-bar colours per category.
static constexpr uint32_t kColEvent  = IM_COL32(160, 60, 60, 255);
static constexpr uint32_t kColFlow   = IM_COL32(60, 60, 160, 255);
static constexpr uint32_t kColMath   = IM_COL32(60, 130, 60, 255);
static constexpr uint32_t kColVec    = IM_COL32(60, 120, 160, 255);
static constexpr uint32_t kColEntity = IM_COL32(150, 100, 40, 255);
static constexpr uint32_t kColProp   = IM_COL32(120, 80, 160, 255);

static constexpr uint32_t kColNodeBg     = IM_COL32(45, 45, 45, 235);
static constexpr uint32_t kColNodeBorder = IM_COL32(100, 100, 100, 200);
static constexpr uint32_t kColSelected   = IM_COL32(255, 165, 0, 255);
static constexpr uint32_t kColGrid       = IM_COL32(60, 60, 60, 255);
static constexpr uint32_t kColGridMajor  = IM_COL32(80, 80, 80, 255);
static constexpr uint32_t kColLinkDraft  = IM_COL32(200, 200, 200, 120);

// ---------------------------------------------------------------------------
// Palette entry — a lightweight descriptor for the node-add palette.
// ---------------------------------------------------------------------------

struct PaletteEntry
{
	const char*            Label;
	NodeScriptPanel::NodeKind Kind;
	const char*            Category;
};

static const PaletteEntry kPalette[] = {
	// Events
	{ "On Pre-Physics",  NodeScriptPanel::NodeKind::Event_OnPrePhysics,  "Events" },
	{ "On Post-Physics", NodeScriptPanel::NodeKind::Event_OnPostPhysics, "Events" },
	{ "On Update",       NodeScriptPanel::NodeKind::Event_OnUpdate,      "Events" },
	{ "On Spawn",        NodeScriptPanel::NodeKind::Event_OnSpawn,       "Events" },
	{ "On Destroy",      NodeScriptPanel::NodeKind::Event_OnDestroy,     "Events" },
	// Flow
	{ "Sequence",        NodeScriptPanel::NodeKind::Sequence,            "Flow"   },
	{ "Branch",          NodeScriptPanel::NodeKind::Branch,              "Flow"   },
	// Properties
	{ "Get Property",    NodeScriptPanel::NodeKind::GetProperty,         "Properties" },
	{ "Set Property",    NodeScriptPanel::NodeKind::SetProperty,         "Properties" },
	// Math
	{ "Add",             NodeScriptPanel::NodeKind::Math_Add,            "Math"   },
	{ "Subtract",        NodeScriptPanel::NodeKind::Math_Subtract,       "Math"   },
	{ "Multiply",        NodeScriptPanel::NodeKind::Math_Multiply,       "Math"   },
	{ "Divide",          NodeScriptPanel::NodeKind::Math_Divide,         "Math"   },
	{ "Clamp",           NodeScriptPanel::NodeKind::Math_Clamp,          "Math"   },
	{ "Lerp",            NodeScriptPanel::NodeKind::Math_Lerp,           "Math"   },
	// Vector
	{ "Make Vec3",       NodeScriptPanel::NodeKind::Vec3_Make,           "Vector" },
	{ "Break Vec3",      NodeScriptPanel::NodeKind::Vec3_Break,          "Vector" },
	{ "Vec3 Length",     NodeScriptPanel::NodeKind::Vec3_Length,         "Vector" },
	{ "Normalize",       NodeScriptPanel::NodeKind::Vec3_Normalize,      "Vector" },
	{ "Vec3 Scale",      NodeScriptPanel::NodeKind::Vec3_Scale,          "Vector" },
	// Entity helpers
	{ "Get Position",    NodeScriptPanel::NodeKind::GetPosition,         "Entity" },
	{ "Set Position",    NodeScriptPanel::NodeKind::SetPosition,         "Entity" },
	{ "Get Velocity",    NodeScriptPanel::NodeKind::GetVelocity,         "Entity" },
	{ "Set Velocity",    NodeScriptPanel::NodeKind::SetVelocity,         "Entity" },
	{ "Apply Impulse",   NodeScriptPanel::NodeKind::ApplyImpulse,        "Entity" },
};
static constexpr int kPaletteSize = static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const char* NodeScriptPanel::NodeKindTitle(NodeKind k)
{
	switch (k)
	{
		case NodeKind::Event_OnPrePhysics:  return "On Pre-Physics";
		case NodeKind::Event_OnPostPhysics: return "On Post-Physics";
		case NodeKind::Event_OnUpdate:      return "On Update";
		case NodeKind::Event_OnSpawn:       return "On Spawn";
		case NodeKind::Event_OnDestroy:     return "On Destroy";
		case NodeKind::Sequence:            return "Sequence";
		case NodeKind::Branch:              return "Branch";
		case NodeKind::GetProperty:         return "Get Property";
		case NodeKind::SetProperty:         return "Set Property";
		case NodeKind::Math_Add:            return "Add";
		case NodeKind::Math_Subtract:       return "Subtract";
		case NodeKind::Math_Multiply:       return "Multiply";
		case NodeKind::Math_Divide:         return "Divide";
		case NodeKind::Math_Clamp:          return "Clamp";
		case NodeKind::Math_Lerp:           return "Lerp";
		case NodeKind::Vec3_Make:           return "Make Vec3";
		case NodeKind::Vec3_Break:          return "Break Vec3";
		case NodeKind::Vec3_Length:         return "Vec3 Length";
		case NodeKind::Vec3_Normalize:      return "Normalize";
		case NodeKind::Vec3_Scale:          return "Vec3 Scale";
		case NodeKind::GetPosition:         return "Get Position";
		case NodeKind::SetPosition:         return "Set Position";
		case NodeKind::GetVelocity:         return "Get Velocity";
		case NodeKind::SetVelocity:         return "Set Velocity";
		case NodeKind::ApplyImpulse:        return "Apply Impulse";
	}
	return "Unknown";
}

static uint32_t NodeKindTitleColor(NodeScriptPanel::NodeKind k)
{
	using NK = NodeScriptPanel::NodeKind;
	switch (k)
	{
		case NK::Event_OnPrePhysics:
		case NK::Event_OnPostPhysics:
		case NK::Event_OnUpdate:
		case NK::Event_OnSpawn:
		case NK::Event_OnDestroy:
			return kColEvent;
		case NK::Sequence:
		case NK::Branch:
			return kColFlow;
		case NK::GetProperty:
		case NK::SetProperty:
			return kColProp;
		case NK::Math_Add:
		case NK::Math_Subtract:
		case NK::Math_Multiply:
		case NK::Math_Divide:
		case NK::Math_Clamp:
		case NK::Math_Lerp:
			return kColMath;
		case NK::Vec3_Make:
		case NK::Vec3_Break:
		case NK::Vec3_Length:
		case NK::Vec3_Normalize:
		case NK::Vec3_Scale:
			return kColVec;
		case NK::GetPosition:
		case NK::SetPosition:
		case NK::GetVelocity:
		case NK::SetVelocity:
		case NK::ApplyImpulse:
			return kColEntity;
	}
	return kColMath;
}

uint32_t NodeScriptPanel::PinTypeColor(PinType t)
{
	switch (t)
	{
		case PinType::Exec:  return IM_COL32(255, 255, 255, 220);
		case PinType::Float: return IM_COL32(140, 230, 140, 220);
		case PinType::Int:   return IM_COL32(100, 160, 255, 220);
		case PinType::Bool:  return IM_COL32(230, 100, 100, 220);
		case PinType::Vec3:  return IM_COL32(255, 200, 80,  220);
	}
	return IM_COL32(200, 200, 200, 220);
}

NodeScriptPanel::ScriptNode* NodeScriptPanel::FindNode(int id)
{
	for (auto& n : Nodes)
		if (n.ID == id) return &n;
	return nullptr;
}

const NodeScriptPanel::ScriptNode* NodeScriptPanel::FindNode(int id) const
{
	for (const auto& n : Nodes)
		if (n.ID == id) return &n;
	return nullptr;
}

const NodeScriptPanel::Pin* NodeScriptPanel::FindPin(int nodeID, int pinID) const
{
	const ScriptNode* node = FindNode(nodeID);
	if (!node) return nullptr;
	for (const auto& p : node->Pins)
		if (p.ID == pinID) return &p;
	return nullptr;
}

// ---------------------------------------------------------------------------
// Node factory helpers
// ---------------------------------------------------------------------------

// Helper: build a Pin value
static NodeScriptPanel::Pin MakePin(int id,
									NodeScriptPanel::PinType type,
									NodeScriptPanel::PinDir  dir,
									const char* name)
{
	NodeScriptPanel::Pin p;
	p.ID   = id;
	p.Type = type;
	p.Dir  = dir;
	snprintf(p.Name, sizeof(p.Name), "%s", name);
	return p;
}

void NodeScriptPanel::AddEventNode(NodeKind kind, float cx, float cy)
{
	ScriptNode n;
	n.ID   = NewID();
	n.Kind = kind;
	n.PosX = cx;
	n.PosY = cy;
	snprintf(n.Title, sizeof(n.Title), "%s", NodeKindTitle(kind));

	// Event nodes have only an exec output (no exec input — they are roots).
	n.Pins.push_back(MakePin(NewID(), PinType::Exec, PinDir::Output, ""));

	Nodes.push_back(std::move(n));
}

void NodeScriptPanel::AddActionNode(NodeKind kind, float cx, float cy)
{
	ScriptNode n;
	n.ID   = NewID();
	n.Kind = kind;
	n.PosX = cx;
	n.PosY = cy;
	snprintf(n.Title, sizeof(n.Title), "%s", NodeKindTitle(kind));

	using PT = PinType;
	using PD = PinDir;

	switch (kind)
	{
		case NodeKind::Sequence:
			n.Pins.push_back(MakePin(NewID(), PT::Exec,  PD::Input,  "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec,  PD::Output, "0"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec,  PD::Output, "1"));
			break;
		case NodeKind::Branch:
			n.Pins.push_back(MakePin(NewID(), PT::Exec,  PD::Input,  "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Bool,  PD::Input,  "Condition"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec,  PD::Output, "True"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec,  PD::Output, "False"));
			break;
		case NodeKind::GetProperty:
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Output, "Value"));
			break;
		case NodeKind::SetProperty:
			n.Pins.push_back(MakePin(NewID(), PT::Exec,  PD::Input,  "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input,  "Value"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec,  PD::Output, "Out"));
			break;
		case NodeKind::Math_Add:
		case NodeKind::Math_Subtract:
		case NodeKind::Math_Multiply:
		case NodeKind::Math_Divide:
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input,  "A"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input,  "B"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Output, "Result"));
			break;
		case NodeKind::Math_Clamp:
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input,  "Value"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input,  "Min"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input,  "Max"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Output, "Result"));
			break;
		case NodeKind::Math_Lerp:
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input,  "A"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input,  "B"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input,  "Alpha"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Output, "Result"));
			break;
		case NodeKind::Vec3_Make:
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input,  "X"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input,  "Y"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input,  "Z"));
			n.Pins.push_back(MakePin(NewID(), PT::Vec3,  PD::Output, "Vec3"));
			break;
		case NodeKind::Vec3_Break:
			n.Pins.push_back(MakePin(NewID(), PT::Vec3,  PD::Input,  "Vec3"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Output, "X"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Output, "Y"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Output, "Z"));
			break;
		case NodeKind::Vec3_Length:
		case NodeKind::Vec3_Normalize:
			n.Pins.push_back(MakePin(NewID(), PT::Vec3,  PD::Input,  "In"));
			n.Pins.push_back(kind == NodeKind::Vec3_Length
				? MakePin(NewID(), PT::Float, PD::Output, "Length")
				: MakePin(NewID(), PT::Vec3,  PD::Output, "Out"));
			break;
		case NodeKind::Vec3_Scale:
			n.Pins.push_back(MakePin(NewID(), PT::Vec3,  PD::Input,  "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input,  "Scale"));
			n.Pins.push_back(MakePin(NewID(), PT::Vec3,  PD::Output, "Out"));
			break;
		case NodeKind::GetPosition:
		case NodeKind::GetVelocity:
			n.Pins.push_back(MakePin(NewID(), PT::Vec3, PD::Output, "Value"));
			break;
		case NodeKind::SetPosition:
		case NodeKind::SetVelocity:
			n.Pins.push_back(MakePin(NewID(), PT::Exec,  PD::Input,  "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Vec3,  PD::Input,  "Value"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec,  PD::Output, "Out"));
			break;
		case NodeKind::ApplyImpulse:
			n.Pins.push_back(MakePin(NewID(), PT::Exec,  PD::Input,  "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Vec3,  PD::Input,  "Impulse"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec,  PD::Output, "Out"));
			break;
		default:
			// Fallback: single exec in/out
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Input,  "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Output, "Out"));
			break;
	}

	Nodes.push_back(std::move(n));
}

// ---------------------------------------------------------------------------
// Constructor — seed the graph with a helpful default
// ---------------------------------------------------------------------------

NodeScriptPanel::NodeScriptPanel()
	: EditorPanel("Node Script")
{
	AddEventNode(NodeKind::Event_OnUpdate, 80.0f, 120.0f);
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

void NodeScriptPanel::DrawGrid(ImDrawList* dl, float canvasX, float canvasY,
							   float canvasW, float canvasH) const
{
	float startX = std::fmod(CanvasScrollX, kGridStep);
	float startY = std::fmod(CanvasScrollY, kGridStep);

	for (float x = startX; x < canvasW; x += kGridStep)
	{
		bool major = std::fmod(x - startX, kGridStep * 4.0f) < 0.5f;
		dl->AddLine({ canvasX + x, canvasY },
					{ canvasX + x, canvasY + canvasH },
					major ? kColGridMajor : kColGrid, 1.0f);
	}
	for (float y = startY; y < canvasH; y += kGridStep)
	{
		bool major = std::fmod(y - startY, kGridStep * 4.0f) < 0.5f;
		dl->AddLine({ canvasX,          canvasY + y },
					{ canvasX + canvasW, canvasY + y },
					major ? kColGridMajor : kColGrid, 1.0f);
	}
}

void NodeScriptPanel::GetPinScreenPos(const ScriptNode& node, const Pin& pin,
									  float canvasX, float canvasY,
									  float& outX, float& outY) const
{
	float nodeScreenX = canvasX + node.PosX + CanvasScrollX;
	float nodeScreenY = canvasY + node.PosY + CanvasScrollY;

	// Count pins by direction to find the row index for this pin.
	bool isOutput = (pin.Dir == PinDir::Output);
	int row = 0;
	for (const auto& p : node.Pins)
	{
		if (p.ID == pin.ID) break;
		if ((p.Dir == PinDir::Output) == isOutput) row++;
	}

	float pinY = nodeScreenY + kNodeTitleH + kPinRowH * static_cast<float>(row) + kPinRowH * 0.5f;
	float pinX = isOutput ? (nodeScreenX + kNodeWidth) : nodeScreenX;

	outX = pinX;
	outY = pinY;
}

bool NodeScriptPanel::HitTestPin(float sx, float sy, float canvasX, float canvasY,
								 int& outNodeID, int& outPinID, bool& outIsOutput) const
{
	for (const auto& node : Nodes)
	{
		for (const auto& pin : node.Pins)
		{
			float px, py;
			GetPinScreenPos(node, pin, canvasX, canvasY, px, py);
			float dx = sx - px;
			float dy = sy - py;
			if (dx * dx + dy * dy <= kPinHitRadius * kPinHitRadius)
			{
				outNodeID   = node.ID;
				outPinID    = pin.ID;
				outIsOutput = (pin.Dir == PinDir::Output);
				return true;
			}
		}
	}
	return false;
}

float NodeScriptPanel::DrawNode(ImDrawList* dl, ScriptNode& node,
								float canvasX, float canvasY)
{
	float sx = canvasX + node.PosX + CanvasScrollX;
	float sy = canvasY + node.PosY + CanvasScrollY;

	// Count inputs and outputs for height calculation.
	int numIn = 0, numOut = 0;
	for (const auto& p : node.Pins)
	{
		if (p.Dir == PinDir::Input)  numIn++;
		else                         numOut++;
	}
	int rows  = std::max(numIn, numOut);
	float bodyH = kNodeTitleH + kPinRowH * static_cast<float>(std::max(rows, 1));
	node.BodyH = bodyH; // Cache for hit-testing next frame.

	bool selected = (node.ID == SelectedNodeID);

	// Shadow
	dl->AddRectFilled({ sx + 4, sy + 4 }, { sx + kNodeWidth + 4, sy + bodyH + 4 },
					  IM_COL32(0, 0, 0, 80), 6.0f);

	// Body
	dl->AddRectFilled({ sx, sy }, { sx + kNodeWidth, sy + bodyH },
					  kColNodeBg, 6.0f);

	// Title bar
	dl->AddRectFilled({ sx, sy }, { sx + kNodeWidth, sy + kNodeTitleH },
					  NodeKindTitleColor(node.Kind), 6.0f);
	dl->AddRectFilled({ sx, sy + kNodeTitleH - 6.0f }, { sx + kNodeWidth, sy + kNodeTitleH },
					  NodeKindTitleColor(node.Kind), 0.0f);

	// Border (highlighted when selected)
	dl->AddRect({ sx, sy }, { sx + kNodeWidth, sy + bodyH },
				selected ? kColSelected : kColNodeBorder, 6.0f, 0, selected ? 2.0f : 1.0f);

	// Title text
	dl->AddText({ sx + 8.0f, sy + 4.0f }, IM_COL32(255, 255, 255, 230), node.Title);

	// Pins
	int inRow = 0, outRow = 0;
	for (const auto& pin : node.Pins)
	{
		bool isOut = (pin.Dir == PinDir::Output);
		int row    = isOut ? outRow++ : inRow++;

		float pinY = sy + kNodeTitleH + kPinRowH * static_cast<float>(row) + kPinRowH * 0.5f;
		float pinX = isOut ? (sx + kNodeWidth) : sx;

		uint32_t col = PinTypeColor(pin.Type);
		dl->AddCircleFilled({ pinX, pinY }, kPinRadius, col);
		dl->AddCircle({ pinX, pinY }, kPinRadius, IM_COL32(0, 0, 0, 160), 12, 1.5f);

		// Pin label
		float labelX = isOut ? (pinX - kPinRadius - 4.0f - ImGui::CalcTextSize(pin.Name).x)
							 : (pinX + kPinRadius + 4.0f);
		if (pin.Name[0] != '\0')
			dl->AddText({ labelX, pinY - 6.0f }, IM_COL32(210, 210, 210, 200), pin.Name);
	}

	return bodyH;
}

void NodeScriptPanel::DrawLinks(ImDrawList* dl, float canvasX, float canvasY)
{
	for (const auto& link : Links)
	{
		const ScriptNode* srcNode = FindNode(link.SrcNodeID);
		const ScriptNode* dstNode = FindNode(link.DstNodeID);
		if (!srcNode || !dstNode) continue;

		const Pin* srcPin = FindPin(link.SrcNodeID, link.SrcPinID);
		const Pin* dstPin = FindPin(link.DstNodeID, link.DstPinID);
		if (!srcPin || !dstPin) continue;

		float sx, sy, ex, ey;
		GetPinScreenPos(*srcNode, *srcPin, canvasX, canvasY, sx, sy);
		GetPinScreenPos(*dstNode, *dstPin, canvasX, canvasY, ex, ey);

		uint32_t col = PinTypeColor(srcPin->Type);
		dl->AddBezierCubic({ sx, sy },
						   { sx + kBezierStrength, sy },
						   { ex - kBezierStrength, ey },
						   { ex, ey },
						   col, 2.0f);
	}
}

// ---------------------------------------------------------------------------
// Code generation
// ---------------------------------------------------------------------------

/// Returns the C++ operator string for math nodes.
static const char* MathOp(NodeScriptPanel::NodeKind k)
{
	switch (k)
	{
		case NodeScriptPanel::NodeKind::Math_Add:      return "+";
		case NodeScriptPanel::NodeKind::Math_Subtract: return "-";
		case NodeScriptPanel::NodeKind::Math_Multiply: return "*";
		case NodeScriptPanel::NodeKind::Math_Divide:   return "/";
		default: return "+";
	}
}

void NodeScriptPanel::GenerateCode()
{
	GeneratedCode.clear();
	StatusMsg[0] = '\0';
	bStatusError  = false;

	// Validate: Branch nodes cannot appear in pre/post-physics graphs.
	bool hasBranch         = false;
	bool hasPrePostPhysics = false;
	for (const auto& n : Nodes)
	{
		if (n.Kind == NodeKind::Branch) hasBranch = true;
		if (n.Kind == NodeKind::Event_OnPrePhysics || n.Kind == NodeKind::Event_OnPostPhysics)
			hasPrePostPhysics = true;
	}
	if (hasBranch && hasPrePostPhysics)
	{
		snprintf(StatusMsg, sizeof(StatusMsg),
				 "ERROR: Branch nodes are forbidden in pre/post-physics graphs (determinism constraint).");
		bStatusError = true;
		GeneratedCode =
			"// Code generation blocked: Branch is not permitted in pre/post-physics.\n"
			"// Remove Branch nodes or move them to an OnUpdate graph.\n";
		return;
	}

	// Build a map: node ID -> indices of links targeting its inputs.
	// We use variable names like _n<id>_<pin> for each output value.
	auto varName = [](int nodeID, int pinID) -> std::string
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "_n%d_p%d", nodeID, pinID);
		return buf;
	};

	// Build adjacency: dst pin -> (src node id, src pin id)
	std::unordered_map<int, std::pair<int, int>> pinSrc; // key = dstPinID, val = (srcNode, srcPin)
	for (const auto& link : Links)
		pinSrc[link.DstPinID] = { link.SrcNodeID, link.SrcPinID };

	// Generate a value expression for an input pin (recursively, inline).
	// Simple approach: only handle data pins (non-Exec). Exec is handled by ordering.
	std::function<std::string(int nodeID, int pinID)> pinExpr;
	pinExpr = [&](int nodeID, int pinID) -> std::string
	{
		auto it = pinSrc.find(pinID);
		if (it == pinSrc.end())
		{
			// No connection — use default values based on pin type.
			const Pin* p = FindPin(nodeID, pinID);
			if (!p) return "0.0f";
			switch (p->Type)
			{
				case PinType::Vec3:  return "{0.0f, 0.0f, 0.0f}";
				case PinType::Bool:  return "false";
				case PinType::Int:   return "0";
				default:             return "0.0f";
			}
		}

		int srcNode = it->second.first;
		int srcPin  = it->second.second;
		const ScriptNode* sn = FindNode(srcNode);
		if (!sn) return "0.0f";

		// For math/vector nodes we inline the expression.
		switch (sn->Kind)
		{
			case NodeKind::Math_Add:
			case NodeKind::Math_Subtract:
			case NodeKind::Math_Multiply:
			case NodeKind::Math_Divide:
			{
				// Find A and B input pins
				int pinA = -1, pinB = -1;
				for (const auto& p : sn->Pins)
					if (p.Dir == PinDir::Input)
					{
						if (pinA == -1) pinA = p.ID;
						else            pinB = p.ID;
					}
				return "(" + pinExpr(srcNode, pinA) + " " + MathOp(sn->Kind) + " " + pinExpr(srcNode, pinB) + ")";
			}
			case NodeKind::Math_Clamp:
			{
				int pVal = -1, pMin = -1, pMax = -1;
				for (const auto& p : sn->Pins)
					if (p.Dir == PinDir::Input)
					{
						if (pVal == -1)      pVal = p.ID;
						else if (pMin == -1) pMin = p.ID;
						else                 pMax = p.ID;
					}
				return "std::clamp(" + pinExpr(srcNode, pVal) + ", " + pinExpr(srcNode, pMin) + ", " + pinExpr(srcNode, pMax) + ")";
			}
			case NodeKind::Math_Lerp:
			{
				int pA = -1, pB = -1, pAlpha = -1;
				for (const auto& p : sn->Pins)
					if (p.Dir == PinDir::Input)
					{
						if (pA == -1)      pA     = p.ID;
						else if (pB == -1) pB     = p.ID;
						else               pAlpha = p.ID;
					}
				return "(" + pinExpr(srcNode, pA) + " + (" + pinExpr(srcNode, pB) + " - " + pinExpr(srcNode, pA) + ") * " + pinExpr(srcNode, pAlpha) + ")";
			}
			case NodeKind::Vec3_Length:
			{
				int pIn = -1;
				for (const auto& p : sn->Pins) if (p.Dir == PinDir::Input) { pIn = p.ID; break; }
				return "glm::length(" + pinExpr(srcNode, pIn) + ")";
			}
			case NodeKind::Vec3_Normalize:
			{
				int pIn = -1;
				for (const auto& p : sn->Pins) if (p.Dir == PinDir::Input) { pIn = p.ID; break; }
				return "glm::normalize(" + pinExpr(srcNode, pIn) + ")";
			}
			case NodeKind::Vec3_Scale:
			{
				int pIn = -1, pScale = -1;
				for (const auto& p : sn->Pins)
					if (p.Dir == PinDir::Input) { if (pIn == -1) pIn = p.ID; else pScale = p.ID; }
				return "(" + pinExpr(srcNode, pIn) + " * " + pinExpr(srcNode, pScale) + ")";
			}
			case NodeKind::Vec3_Make:
			{
				int pX = -1, pY = -1, pZ = -1;
				for (const auto& p : sn->Pins)
					if (p.Dir == PinDir::Input)
					{
						if (pX == -1)      pX = p.ID;
						else if (pY == -1) pY = p.ID;
						else               pZ = p.ID;
					}
				return "glm::vec3(" + pinExpr(srcNode, pX) + ", " + pinExpr(srcNode, pY) + ", " + pinExpr(srcNode, pZ) + ")";
			}
			case NodeKind::GetPosition:
				return "glm::vec3(Transform.PosX, Transform.PosY, Transform.PosZ)";
			case NodeKind::GetVelocity:
				return "glm::vec3(Velocity.X, Velocity.Y, Velocity.Z)";
			case NodeKind::GetProperty:
				return varName(srcNode, srcPin);
			default:
				return varName(srcNode, srcPin);
		}
	};

	// Generate an exec chain from a given exec output pin.
	std::function<std::string(int srcNodeID, int srcExecPinID, int indent)> genExec;
	genExec = [&](int srcNodeID, int srcExecPinID, int indent) -> std::string
	{
		// Find which node/pin this exec output connects to.
		const NodeLink* foundLink = nullptr;
		for (const auto& link : Links)
		{
			if (link.SrcNodeID == srcNodeID && link.SrcPinID == srcExecPinID)
			{
				foundLink = &link;
				break;
			}
		}
		if (!foundLink) return "";

		const ScriptNode* dst = FindNode(foundLink->DstNodeID);
		if (!dst) return "";

		std::string pad(static_cast<size_t>(indent) * 4, ' ');
		std::string body;

		// Find this node's exec output pin for chaining.
		int nextExecSrc = -1;
		for (const auto& p : dst->Pins)
		{
			if (p.Dir == PinDir::Output && p.Type == PinType::Exec)
			{
				nextExecSrc = p.ID;
				break;
			}
		}

		switch (dst->Kind)
		{
			case NodeKind::SetPosition:
			{
				int pVal = -1;
				for (const auto& p : dst->Pins) if (p.Dir == PinDir::Input && p.Type == PinType::Vec3) { pVal = p.ID; break; }
				std::string val = pinExpr(dst->ID, pVal);
				body += pad + "{\n";
				body += pad + "    auto _pos = " + val + ";\n";
				body += pad + "    Transform.PosX = _pos.x;\n";
				body += pad + "    Transform.PosY = _pos.y;\n";
				body += pad + "    Transform.PosZ = _pos.z;\n";
				body += pad + "}\n";
				break;
			}
			case NodeKind::SetVelocity:
			{
				int pVal = -1;
				for (const auto& p : dst->Pins) if (p.Dir == PinDir::Input && p.Type == PinType::Vec3) { pVal = p.ID; break; }
				std::string val = pinExpr(dst->ID, pVal);
				body += pad + "{\n";
				body += pad + "    auto _vel = " + val + ";\n";
				body += pad + "    Velocity.X = _vel.x;\n";
				body += pad + "    Velocity.Y = _vel.y;\n";
				body += pad + "    Velocity.Z = _vel.z;\n";
				body += pad + "}\n";
				break;
			}
			case NodeKind::ApplyImpulse:
			{
				int pImp = -1;
				for (const auto& p : dst->Pins) if (p.Dir == PinDir::Input && p.Type == PinType::Vec3) { pImp = p.ID; break; }
				std::string val = pinExpr(dst->ID, pImp);
				body += pad + "GetPhysics()->ApplyImpulse(BodyID, " + val + ");\n";
				break;
			}
			case NodeKind::SetProperty:
			{
				int pVal = -1;
				for (const auto& p : dst->Pins) if (p.Dir == PinDir::Input && p.Type != PinType::Exec) { pVal = p.ID; break; }
				body += pad + varName(dst->ID, 0) + " = " + pinExpr(dst->ID, pVal) + ";\n";
				break;
			}
			case NodeKind::Sequence:
			{
				// Emit each output branch in order.
				for (const auto& p : dst->Pins)
					if (p.Dir == PinDir::Output && p.Type == PinType::Exec)
						body += genExec(dst->ID, p.ID, indent);
				break;
			}
			default:
				break;
		}

		// Chain to next exec node.
		if (nextExecSrc != -1)
			body += genExec(dst->ID, nextExecSrc, indent);

		return body;
	};

	// Walk all event nodes and generate corresponding update/lifetime function bodies.
	std::string out;
	out += "#pragma once\n";
	out += "// Auto-generated by Trinyx Node Script — do not edit by hand.\n";
	out += "// Re-open the graph in Node Script and regenerate to update.\n\n";
	out += "#include \"Construct.h\"\n";
	out += "#include \"ConstructView.h\"\n\n";

	// Derive entity include from TargetEntityName
	out += std::string("#include \"") + TargetEntityName + ".h\"\n\n";
	out += std::string("class ") + TargetEntityName + "Script : public Construct<" + TargetEntityName + "Script>\n{\n";
	out += std::string("    ConstructView<") + TargetEntityName + "> Body;\n\n";
	out += "public:\n";
	out += "    void InitializeViews() { Body.Initialize(this); }\n\n";

	for (const auto& event : Nodes)
	{
		if (!event.IsEventNode()) continue;

		// Determine method name and signature.
		const char* methodName = nullptr;
		const char* sig        = nullptr;
		switch (event.Kind)
		{
			case NodeKind::Event_OnPrePhysics:  methodName = "PrePhysics";  sig = "SimFloat dt"; break;
			case NodeKind::Event_OnPostPhysics: methodName = "PostPhysics"; sig = "SimFloat dt"; break;
			case NodeKind::Event_OnUpdate:      methodName = "ScalarUpdate"; sig = "float dt"; break;
			case NodeKind::Event_OnSpawn:       methodName = "OnSpawn";     sig = ""; break;
			case NodeKind::Event_OnDestroy:     methodName = "OnDestroy";   sig = ""; break;
			default: continue;
		}

		out += std::string("    void ") + methodName + "(" + sig + ")\n    {\n";

		// Find the exec output pin of the event node.
		for (const auto& p : event.Pins)
		{
			if (p.Dir == PinDir::Output && p.Type == PinType::Exec)
			{
				out += genExec(event.ID, p.ID, 2);
				break;
			}
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
		LOG_ERROR_F("[NodeScript] Failed to create output directory: %s", ex.what());
	}

	std::ofstream file(OutputPath);
	if (!file.is_open())
	{
		snprintf(StatusMsg, sizeof(StatusMsg), "Failed to open: %s", OutputPath);
		bStatusError = true;
		return false;
	}

	file << GeneratedCode;
	snprintf(StatusMsg, sizeof(StatusMsg), "Exported: %s", OutputPath);
	bStatusError = false;
	LOG_INFO_F("[NodeScript] Exported script -> %s", OutputPath);
	return true;
}

// ---------------------------------------------------------------------------
// Main Draw
// ---------------------------------------------------------------------------

void NodeScriptPanel::Draw(EditorState& state)
{
	ImGui::Begin(Title, &bVisible, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	// Auto-suggest output path from the project directory on the first frame it becomes available.
	if (OutputPath[0] == '\0' && state.ConfigPtr && state.ConfigPtr->ProjectDir[0] != '\0')
	{
		snprintf(OutputPath, sizeof(OutputPath),
				 "%s/src/Scripted/%sScript.h",
				 state.ConfigPtr->ProjectDir, TargetEntityName);
	}

	// -----------------------------------------------------------------------
	// Top toolbar
	// -----------------------------------------------------------------------
	ImGui::SetNextItemWidth(160.0f);
	if (ImGui::InputText("Entity##TargetName", TargetEntityName, sizeof(TargetEntityName)))
	{
		// Refresh the suggested output path when the entity name changes.
		if (state.ConfigPtr && state.ConfigPtr->ProjectDir[0] != '\0')
		{
			snprintf(OutputPath, sizeof(OutputPath),
					 "%s/src/Scripted/%sScript.h",
					 state.ConfigPtr->ProjectDir, TargetEntityName);
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

	// -----------------------------------------------------------------------
	// Split: palette | canvas | (optional) code preview
	// -----------------------------------------------------------------------
	float totalW      = ImGui::GetContentRegionAvail().x;
	float paletteW    = 160.0f;
	float codeW       = bShowCodePreview ? 280.0f : 0.0f;
	float canvasW     = totalW - paletteW - codeW - (bShowCodePreview ? 8.0f : 4.0f);
	float canvasH     = ImGui::GetContentRegionAvail().y;

	// --- Palette ---
	ImGui::BeginChild("##Palette", ImVec2(paletteW, canvasH), ImGuiChildFlags_None);
	ImGui::Text("Node Palette");
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputText("##PalFilter", PaletteFilter, sizeof(PaletteFilter));
	ImGui::Separator();

	const char* currentCategory = nullptr;
	for (int i = 0; i < kPaletteSize; i++)
	{
		const PaletteEntry& entry = kPalette[i];

		// Apply text filter
		if (PaletteFilter[0] != '\0')
		{
			// Case-insensitive substring search
			std::string lower = entry.Label;
			std::string filter = PaletteFilter;
			for (auto& c : lower)   c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			for (auto& c : filter)  c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			if (lower.find(filter) == std::string::npos) continue;
		}

		// Category header
		if (!currentCategory || strcmp(currentCategory, entry.Category) != 0)
		{
			currentCategory = entry.Category;
			ImGui::TextDisabled("%s", currentCategory);
		}

		ImGui::Indent(8.0f);
		bool clicked = ImGui::Button(entry.Label, ImVec2(paletteW - 24.0f, 0));
		ImGui::Unindent(8.0f);

		if (clicked)
		{
			// Place new node near the centre of the current canvas view.
			float cx = -CanvasScrollX + canvasW * 0.5f - kNodeWidth * 0.5f;
			float cy = -CanvasScrollY + canvasH * 0.5f - kNodeTitleH;
			if (entry.Kind <= NodeKind::Event_OnDestroy)
				AddEventNode(entry.Kind, cx, cy);
			else
				AddActionNode(entry.Kind, cx, cy);
		}
	}
	ImGui::EndChild();

	ImGui::SameLine();

	// --- Node canvas ---
	ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##Canvas", ImVec2(canvasW, canvasH),
						   ImGuiButtonFlags_MouseButtonLeft
						   | ImGuiButtonFlags_MouseButtonRight
						   | ImGuiButtonFlags_MouseButtonMiddle);

	bool canvasHovered = ImGui::IsItemHovered();
	bool canvasActive  = ImGui::IsItemActive();

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(canvasOrigin, { canvasOrigin.x + canvasW, canvasOrigin.y + canvasH }, true);

	// Background
	dl->AddRectFilled(canvasOrigin,
					  { canvasOrigin.x + canvasW, canvasOrigin.y + canvasH },
					  IM_COL32(30, 30, 30, 255));

	DrawGrid(dl, canvasOrigin.x, canvasOrigin.y, canvasW, canvasH);

	DrawLinks(dl, canvasOrigin.x, canvasOrigin.y);

	// --- Interaction: node dragging / selection ---
	ImGuiIO& io = ImGui::GetIO();

	if (canvasActive && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		float mx = io.MousePos.x;
		float my = io.MousePos.y;

		if (!bDraggingNode && !bCreatingLink)
		{
			// Hit-test pins first (for starting a link).
			int hitNode, hitPin;
			bool hitOut;
			if (HitTestPin(mx, my, canvasOrigin.x, canvasOrigin.y, hitNode, hitPin, hitOut))
			{
				if (!bCreatingLink)
				{
					bCreatingLink    = true;
					LinkSrcNodeID    = hitNode;
					LinkSrcPinID     = hitPin;
					bLinkSrcIsOutput = hitOut;
				}
			}
			else
			{
				// Hit-test nodes for selection / drag.
				bool hitAny = false;
				for (auto& node : Nodes)
				{
					float nx = canvasOrigin.x + node.PosX + CanvasScrollX;
					float ny = canvasOrigin.y + node.PosY + CanvasScrollY;
					if (mx >= nx && mx <= nx + kNodeWidth && my >= ny && my <= ny + node.BodyH)
					{
						SelectedNodeID = node.ID;
						bDraggingNode  = true;
						DragOffsetX    = mx - nx;
						DragOffsetY    = my - ny;
						hitAny         = true;
						break;
					}
				}
				if (!hitAny) SelectedNodeID = -1;
			}
		}

		if (bDraggingNode)
		{
			ScriptNode* sel = FindNode(SelectedNodeID);
			if (sel)
			{
				sel->PosX = mx - canvasOrigin.x - CanvasScrollX - DragOffsetX;
				sel->PosY = my - canvasOrigin.y - CanvasScrollY - DragOffsetY;
			}
		}

		if (bCreatingLink)
		{
			// Draw a draft bezier from the source pin to the mouse.
			bool srcIsOut = false;
			float srcX = 0, srcY = 0;
			{
				const ScriptNode* sn = FindNode(LinkSrcNodeID);
				if (sn)
				{
					for (const auto& p : sn->Pins)
					{
						if (p.ID == LinkSrcPinID)
						{
							GetPinScreenPos(*sn, p, canvasOrigin.x, canvasOrigin.y, srcX, srcY);
							srcIsOut = (p.Dir == PinDir::Output);
							break;
						}
					}
				}
			}
			float ex = srcIsOut ? mx : srcX;
			float ey = srcIsOut ? my : srcY;
			float bx = srcIsOut ? srcX : mx;
			float by = srcIsOut ? srcY : my;
			dl->AddBezierCubic({ bx, by },
							   { bx + kBezierStrength, by },
							   { ex - kBezierStrength, ey },
							   { ex, ey },
							   kColLinkDraft, 1.5f);
		}
	}

	if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
	{
		bDraggingNode = false;

		if (bCreatingLink)
		{
			float mx = io.MousePos.x;
			float my = io.MousePos.y;
			int dstNode, dstPin;
			bool dstIsOut;
			if (HitTestPin(mx, my, canvasOrigin.x, canvasOrigin.y, dstNode, dstPin, dstIsOut))
			{
				// Validate: must connect output → input.
				bool valid = (bLinkSrcIsOutput != dstIsOut) && (dstNode != LinkSrcNodeID);
				if (valid)
				{
					int srcNode = bLinkSrcIsOutput ? LinkSrcNodeID : dstNode;
					int srcPin  = bLinkSrcIsOutput ? LinkSrcPinID  : dstPin;
					int dNode   = bLinkSrcIsOutput ? dstNode : LinkSrcNodeID;
					int dPin    = bLinkSrcIsOutput ? dstPin  : LinkSrcPinID;

					// Remove existing link to the same input pin (one input per pin).
					Links.erase(std::remove_if(Links.begin(), Links.end(),
						[dPin](const NodeLink& l) { return l.DstPinID == dPin; }),
						Links.end());

					NodeLink link;
					link.ID        = NewID();
					link.SrcNodeID = srcNode;
					link.SrcPinID  = srcPin;
					link.DstNodeID = dNode;
					link.DstPinID  = dPin;
					Links.push_back(link);
				}
			}
			bCreatingLink = false;
		}
	}

	// Middle-mouse or right-mouse + drag: pan canvas.
	if (canvasHovered && ImGui::IsMouseDown(ImGuiMouseButton_Middle))
	{
		CanvasScrollX += io.MouseDelta.x;
		CanvasScrollY += io.MouseDelta.y;
	}

	// Context menu on right-click on empty canvas.
	if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
	{
		float mx = io.MousePos.x - canvasOrigin.x;
		float my = io.MousePos.y - canvasOrigin.y;

		// Only show context menu if not over a node.
		bool overNode = false;
		for (const auto& node : Nodes)
		{
			float nx = node.PosX + CanvasScrollX;
			float ny = node.PosY + CanvasScrollY;
			if (mx >= nx && mx <= nx + kNodeWidth && my >= ny && my <= ny + node.BodyH)
			{
				overNode = true;
				break;
			}
		}
		if (!overNode)
		{
			bShowContextMenu   = true;
			ContextMenuCanvasX = mx - CanvasScrollX;
			ContextMenuCanvasY = my - CanvasScrollY;
			ImGui::OpenPopup("##NodeCtxMenu");
		}
		else
		{
			// Right-click on node: delete via Del key hint is shown in properties.
		}
	}

	// Delete selected node.
	if (SelectedNodeID != -1 && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
	{
		Links.erase(std::remove_if(Links.begin(), Links.end(),
			[this](const NodeLink& l) {
				return l.SrcNodeID == SelectedNodeID || l.DstNodeID == SelectedNodeID;
			}), Links.end());
		Nodes.erase(std::remove_if(Nodes.begin(), Nodes.end(),
			[this](const ScriptNode& n) { return n.ID == SelectedNodeID; }), Nodes.end());
		SelectedNodeID = -1;
	}

	// Context menu popup.
	if (ImGui::BeginPopup("##NodeCtxMenu"))
	{
		ImGui::Text("Add Node");
		ImGui::Separator();

		const char* lastCat = nullptr;
		for (int i = 0; i < kPaletteSize; i++)
		{
			const PaletteEntry& entry = kPalette[i];
			if (!lastCat || strcmp(lastCat, entry.Category) != 0)
			{
				if (lastCat) ImGui::Separator();
				lastCat = entry.Category;
				ImGui::TextDisabled("%s", lastCat);
			}
			if (ImGui::MenuItem(entry.Label))
			{
				if (entry.Kind <= NodeKind::Event_OnDestroy)
					AddEventNode(entry.Kind, ContextMenuCanvasX, ContextMenuCanvasY);
				else
					AddActionNode(entry.Kind, ContextMenuCanvasX, ContextMenuCanvasY);
			}
		}
		ImGui::EndPopup();
	}

	// Draw all nodes (on top of links).
	for (auto& node : Nodes)
		DrawNode(dl, node, canvasOrigin.x, canvasOrigin.y);

	dl->PopClipRect();

	// --- Code preview pane ---
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
