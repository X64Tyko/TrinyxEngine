#include "NodeGraphCanvas.h"
#if !defined(TNX_ENABLE_EDITOR)
#error "NodeGraphCanvas.cpp requires TNX_ENABLE_EDITOR"
#endif

#include "TnxStyle.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr float NodeWidth      = 190.0f;
static constexpr float NodeTitleH     = 24.0f;
static constexpr float PinRowH        = 20.0f;
static constexpr float NodePadY       = 5.0f; // gap above first pin row and below last
static constexpr float PinRadius      = 5.0f;
static constexpr float PinHitRadius   = 8.0f;
static constexpr float GridStep       = 48.0f;
static constexpr float BezierStrength = 80.0f;

static constexpr uint32_t ColEvent  = IM_COL32(160, 60, 60, 255);
static constexpr uint32_t ColFlow   = IM_COL32(60, 60, 160, 255);
static constexpr uint32_t ColMath   = IM_COL32(60, 130, 60, 255);
static constexpr uint32_t ColVec    = IM_COL32(60, 120, 160, 255);
static constexpr uint32_t ColEntity = IM_COL32(150, 100, 40, 255);
static constexpr uint32_t ColProp   = IM_COL32(120, 80, 160, 255);

static constexpr uint32_t ColNodeBg     = IM_COL32(45, 45, 45, 235);
static constexpr uint32_t ColNodeBorder = IM_COL32(100, 100, 100, 200);
static constexpr uint32_t ColSelected   = IM_COL32(255, 165, 0, 255);
static constexpr uint32_t ColGrid       = IM_COL32(60, 60, 60, 255);
static constexpr uint32_t ColGridMajor  = IM_COL32(80, 80, 80, 255);
static constexpr uint32_t ColLinkDraft  = IM_COL32(200, 200, 200, 120);

// ---------------------------------------------------------------------------
// Palette table
// ---------------------------------------------------------------------------

struct PaletteEntry
{
	const char* Label;
	NodeGraphCanvas::NodeKind Kind;
	const char* Category;
};

static const PaletteEntry Palette[] = {
	// Events
	{ "On Pre-Physics", NodeGraphCanvas::NodeKind::Event_OnPrePhysics, "Events" },
	{ "On Physics Step", NodeGraphCanvas::NodeKind::Event_OnPhysicsStep, "Events" },
	{ "On Post-Physics", NodeGraphCanvas::NodeKind::Event_OnPostPhysics, "Events" },
	{ "On Update", NodeGraphCanvas::NodeKind::Event_OnUpdate, "Events" },
	{ "On Spawn", NodeGraphCanvas::NodeKind::Event_OnSpawn, "Events" },
	{ "On Destroy", NodeGraphCanvas::NodeKind::Event_OnDestroy, "Events" },
	// Flow
	{ "Sequence", NodeGraphCanvas::NodeKind::Sequence, "Flow" },
	{ "Branch", NodeGraphCanvas::NodeKind::Branch, "Flow" },
	{ "Return", NodeGraphCanvas::NodeKind::Return_, "Flow" },
	// Properties
	{ "Get Property", NodeGraphCanvas::NodeKind::GetProperty, "Properties" },
	{ "Set Property", NodeGraphCanvas::NodeKind::SetProperty, "Properties" },
	// Math
	{ "Add", NodeGraphCanvas::NodeKind::Math_Add, "Math" },
	{ "Subtract", NodeGraphCanvas::NodeKind::Math_Subtract, "Math" },
	{ "Multiply", NodeGraphCanvas::NodeKind::Math_Multiply, "Math" },
	{ "Divide", NodeGraphCanvas::NodeKind::Math_Divide, "Math" },
	{ "Clamp", NodeGraphCanvas::NodeKind::Math_Clamp, "Math" },
	{ "Lerp", NodeGraphCanvas::NodeKind::Math_Lerp, "Math" },
	{ "Sqrt", NodeGraphCanvas::NodeKind::Math_Sqrt, "Math" },
	{ "Abs", NodeGraphCanvas::NodeKind::Math_Abs, "Math" },
	{ "Negate", NodeGraphCanvas::NodeKind::Math_Negate, "Math" },
	// Logic
	{ "Compare", NodeGraphCanvas::NodeKind::Compare, "Logic" },
	{ "And", NodeGraphCanvas::NodeKind::LogicalAnd, "Logic" },
	{ "Or", NodeGraphCanvas::NodeKind::LogicalOr, "Logic" },
	{ "Not", NodeGraphCanvas::NodeKind::LogicalNot, "Logic" },
	// Vector
	{ "Make Vec3", NodeGraphCanvas::NodeKind::Vec3_Make, "Vector" },
	{ "Break Vec3", NodeGraphCanvas::NodeKind::Vec3_Break, "Vector" },
	{ "Vec3 Length", NodeGraphCanvas::NodeKind::Vec3_Length, "Vector" },
	{ "Normalize", NodeGraphCanvas::NodeKind::Vec3_Normalize, "Vector" },
	{ "Vec3 Scale", NodeGraphCanvas::NodeKind::Vec3_Scale, "Vector" },
	// Entity
	{ "Get Position", NodeGraphCanvas::NodeKind::GetPosition, "Entity" },
	{ "Set Position", NodeGraphCanvas::NodeKind::SetPosition, "Entity" },
	{ "Get Velocity", NodeGraphCanvas::NodeKind::GetVelocity, "Entity" },
	{ "Set Velocity", NodeGraphCanvas::NodeKind::SetVelocity, "Entity" },
	{ "Apply Impulse", NodeGraphCanvas::NodeKind::ApplyImpulse, "Entity" },
	// Input
	{ "Check Action", NodeGraphCanvas::NodeKind::CheckAction, "Input" },
	// Variables
	{ "Local Var", NodeGraphCanvas::NodeKind::LocalVar, "Variables" },
	// Values
	{ "Float Const", NodeGraphCanvas::NodeKind::Const_Float, "Values" },
	{ "Int Const", NodeGraphCanvas::NodeKind::Const_Int, "Values" },
	{ "Bool Const", NodeGraphCanvas::NodeKind::Const_Bool, "Values" },
	{ "Get Dt", NodeGraphCanvas::NodeKind::GetDeltaTime, "Values" },
};
static constexpr int PaletteSize = static_cast<int>(sizeof(Palette) / sizeof(Palette[0]));

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const char* NodeGraphCanvas::NodeKindTitle(NodeKind k)
{
	switch (k)
	{
		case NodeKind::Event_OnPrePhysics: return "On Pre-Physics";
		case NodeKind::Event_OnPhysicsStep: return "On Physics Step";
		case NodeKind::Event_OnPostPhysics: return "On Post-Physics";
		case NodeKind::Event_OnUpdate: return "On Update";
		case NodeKind::Event_OnSpawn: return "On Spawn";
		case NodeKind::Event_OnDestroy: return "On Destroy";
		case NodeKind::Sequence: return "Sequence";
		case NodeKind::Branch: return "Branch";
		case NodeKind::GetProperty: return "Get Property";
		case NodeKind::SetProperty: return "Set Property";
		case NodeKind::Math_Add: return "Add";
		case NodeKind::Math_Subtract: return "Subtract";
		case NodeKind::Math_Multiply: return "Multiply";
		case NodeKind::Math_Divide: return "Divide";
		case NodeKind::Math_Clamp: return "Clamp";
		case NodeKind::Math_Lerp: return "Lerp";
		case NodeKind::Vec3_Make: return "Make Vec3";
		case NodeKind::Vec3_Break: return "Break Vec3";
		case NodeKind::Vec3_Length: return "Vec3 Length";
		case NodeKind::Vec3_Normalize: return "Normalize";
		case NodeKind::Vec3_Scale: return "Vec3 Scale";
		case NodeKind::GetPosition: return "Get Position";
		case NodeKind::SetPosition: return "Set Position";
		case NodeKind::GetVelocity: return "Get Velocity";
		case NodeKind::SetVelocity: return "Set Velocity";
		case NodeKind::ApplyImpulse: return "Apply Impulse";
		case NodeKind::Const_Float: return "Float";
		case NodeKind::Const_Int: return "Int";
		case NodeKind::Const_Bool: return "Bool";
		case NodeKind::GetDeltaTime: return "Get Dt";
		case NodeKind::Compare: return "Compare";
		case NodeKind::LogicalAnd: return "And";
		case NodeKind::LogicalOr: return "Or";
		case NodeKind::LogicalNot: return "Not";
		case NodeKind::CheckAction: return "Check Action";
		case NodeKind::Math_Sqrt: return "Sqrt";
		case NodeKind::Math_Abs: return "Abs";
		case NodeKind::Math_Negate: return "Negate";
		case NodeKind::Return_: return "Return";
		case NodeKind::LocalVar: return "Local";
		case NodeKind::FuncEntry: return "Function";
		case NodeKind::RawCode: return "Code";
	}
	return "Unknown";
}

static uint32_t NodeKindTitleColor(NodeGraphCanvas::NodeKind k)
{
	using NK = NodeGraphCanvas::NodeKind;
	switch (k)
	{
		case NK::Event_OnPrePhysics:
		case NK::Event_OnPhysicsStep:
		case NK::Event_OnPostPhysics:
		case NK::Event_OnUpdate:
		case NK::Event_OnSpawn:
		case NK::Event_OnDestroy:
			return ColEvent;
		case NK::Sequence:
		case NK::Branch:
			return ColFlow;
		case NK::GetProperty:
		case NK::SetProperty:
			return ColProp;
		case NK::Math_Add:
		case NK::Math_Subtract:
		case NK::Math_Multiply:
		case NK::Math_Divide:
		case NK::Math_Clamp:
		case NK::Math_Lerp:
			return ColMath;
		case NK::Vec3_Make:
		case NK::Vec3_Break:
		case NK::Vec3_Length:
		case NK::Vec3_Normalize:
		case NK::Vec3_Scale:
			return ColVec;
		case NK::GetPosition:
		case NK::SetPosition:
		case NK::GetVelocity:
		case NK::SetVelocity:
		case NK::ApplyImpulse:
			return ColEntity;
		case NK::Math_Sqrt:
		case NK::Math_Abs:
		case NK::Math_Negate:
			return ColMath;
		case NK::Compare:
		case NK::LogicalAnd:
		case NK::LogicalOr:
		case NK::LogicalNot:
		case NK::Return_:
			return ColFlow;
		case NK::CheckAction:
			return ColVec;
		case NK::Const_Float:
		case NK::Const_Int:
		case NK::Const_Bool:
		case NK::GetDeltaTime:
			return IM_COL32(70, 70, 70, 255);
		case NK::LocalVar:
			return IM_COL32(55, 75, 95, 255);
		case NK::FuncEntry:
			return IM_COL32(40, 120, 140, 255);
		case NK::RawCode:
			return IM_COL32(80, 55, 80, 255);
	}
	return ColMath;
}

uint32_t NodeGraphCanvas::PinTypeColor(PinType t)
{
	switch (t)
	{
		case PinType::Exec: return IM_COL32(255, 255, 255, 220);
		case PinType::Float: return IM_COL32(140, 230, 140, 220);
		case PinType::Int: return IM_COL32(100, 160, 255, 220);
		case PinType::Bool: return IM_COL32(230, 100, 100, 220);
		case PinType::Vec3: return IM_COL32(255, 200, 80, 220);
		case PinType::Any: return IM_COL32(180, 180, 180, 200);
	}
	return IM_COL32(200, 200, 200, 220);
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

NodeGraphCanvas::ScriptNode* NodeGraphCanvas::FindNode(int id)
{
	for (auto& n : Nodes)
		if (n.ID == id) return &n;
	return nullptr;
}

const NodeGraphCanvas::ScriptNode* NodeGraphCanvas::FindNode(int id) const
{
	for (const auto& n : Nodes)
		if (n.ID == id) return &n;
	return nullptr;
}

const NodeGraphCanvas::Pin* NodeGraphCanvas::FindPin(int nodeID, int pinID) const
{
	const ScriptNode* node = FindNode(nodeID);
	if (!node) return nullptr;
	for (const auto& p : node->Pins)
		if (p.ID == pinID) return &p;
	return nullptr;
}

// ---------------------------------------------------------------------------
// Node factory
// ---------------------------------------------------------------------------

static NodeGraphCanvas::Pin MakePin(int id,
	NodeGraphCanvas::PinType type,
	NodeGraphCanvas::PinDir dir,
	const char* name)
{
	NodeGraphCanvas::Pin p;
	p.ID   = id;
	p.Type = type;
	p.Dir  = dir;
	snprintf(p.Name, sizeof(p.Name), "%s", name);
	return p;
}

NodeGraphCanvas::NodeGraphCanvas()
{
	AddDefaultEventNode();
}

void NodeGraphCanvas::AddDefaultEventNode()
{
	AddEventNode(NodeKind::Event_OnUpdate, 80.0f, 120.0f);
}

void NodeGraphCanvas::Clear()
{
	Nodes.clear();
	Links.clear();
	NextID         = 1;
	CanvasScrollX  = 0.0f;
	CanvasScrollY  = 0.0f;
	SelectedNodeID = -1;
	bDraggingNode  = false;
	bCreatingLink  = false;
}

void NodeGraphCanvas::AddEventNode(NodeKind kind, float cx, float cy)
{
	ScriptNode n;
	n.ID   = NewID();
	n.Kind = kind;
	n.PosX = cx;
	n.PosY = cy;
	snprintf(n.Title, sizeof(n.Title), "%s", NodeKindTitle(kind));
	n.Pins.push_back(MakePin(NewID(), PinType::Exec, PinDir::Output, ""));
	Nodes.push_back(std::move(n));
}

void NodeGraphCanvas::AddFuncEntryNode(const char* displayName, float cx, float cy)
{
	ScriptNode n;
	n.ID   = NewID();
	n.Kind = NodeKind::FuncEntry;
	n.PosX = cx;
	n.PosY = cy;
	snprintf(n.Title, sizeof(n.Title), "%s", displayName);
	n.Pins.push_back(MakePin(NewID(), PinType::Exec, PinDir::Output, ""));
	Nodes.push_back(std::move(n));
}

void NodeGraphCanvas::AddActionNode(NodeKind kind, float cx, float cy)
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
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Input, "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Output, "0"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Output, "1"));
			break;
		case NodeKind::Branch:
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Input, "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Bool, PD::Input, "Condition"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Output, "True"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Output, "False"));
			break;
		case NodeKind::GetProperty:
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Output, "Value"));
			break;
		case NodeKind::SetProperty:
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Input, "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input, "Value"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Output, "Out"));
			break;
		case NodeKind::Math_Add:
		case NodeKind::Math_Subtract:
		case NodeKind::Math_Multiply:
		case NodeKind::Math_Divide:
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input, "A"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input, "B"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Output, "Result"));
			break;
		case NodeKind::Math_Clamp:
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input, "Value"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input, "Min"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input, "Max"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Output, "Result"));
			break;
		case NodeKind::Math_Lerp:
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input, "A"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input, "B"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input, "Alpha"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Output, "Result"));
			break;
		case NodeKind::Vec3_Make:
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input, "X"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input, "Y"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input, "Z"));
			n.Pins.push_back(MakePin(NewID(), PT::Vec3, PD::Output, "Vec3"));
			break;
		case NodeKind::Vec3_Break:
			n.Pins.push_back(MakePin(NewID(), PT::Vec3, PD::Input, "Vec3"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Output, "X"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Output, "Y"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Output, "Z"));
			break;
		case NodeKind::Vec3_Length:
		case NodeKind::Vec3_Normalize:
			n.Pins.push_back(MakePin(NewID(), PT::Vec3, PD::Input, "In"));
			n.Pins.push_back(kind == NodeKind::Vec3_Length
								 ? MakePin(NewID(), PT::Float, PD::Output, "Length")
								 : MakePin(NewID(), PT::Vec3, PD::Output, "Out"));
			break;
		case NodeKind::Vec3_Scale:
			n.Pins.push_back(MakePin(NewID(), PT::Vec3, PD::Input, "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input, "Scale"));
			n.Pins.push_back(MakePin(NewID(), PT::Vec3, PD::Output, "Out"));
			break;
		case NodeKind::GetPosition:
		case NodeKind::GetVelocity:
			n.Pins.push_back(MakePin(NewID(), PT::Vec3, PD::Output, "Value"));
			break;
		case NodeKind::SetPosition:
		case NodeKind::SetVelocity:
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Input, "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Vec3, PD::Input, "Value"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Output, "Out"));
			break;
		case NodeKind::ApplyImpulse:
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Input, "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Vec3, PD::Input, "Impulse"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Output, "Out"));
			break;
		case NodeKind::Const_Float:
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Output, "Value"));
			break;
		case NodeKind::Const_Int:
			n.Pins.push_back(MakePin(NewID(), PT::Int, PD::Output, "Value"));
			break;
		case NodeKind::Const_Bool:
			n.Pins.push_back(MakePin(NewID(), PT::Bool, PD::Output, "Value"));
			break;
		case NodeKind::GetDeltaTime:
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Output, "dt"));
			break;
		case NodeKind::Compare:
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input, "A"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input, "B"));
			n.Pins.push_back(MakePin(NewID(), PT::Bool, PD::Output, "Result"));
			break;
		case NodeKind::LogicalAnd:
		case NodeKind::LogicalOr:
			n.Pins.push_back(MakePin(NewID(), PT::Bool, PD::Input, "A"));
			n.Pins.push_back(MakePin(NewID(), PT::Bool, PD::Input, "B"));
			n.Pins.push_back(MakePin(NewID(), PT::Bool, PD::Output, "Result"));
			break;
		case NodeKind::LogicalNot:
			n.Pins.push_back(MakePin(NewID(), PT::Bool, PD::Input, "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Bool, PD::Output, "Out"));
			break;
		case NodeKind::CheckAction:
			n.Pins.push_back(MakePin(NewID(), PT::Bool, PD::Output, "Pressed"));
			break;
		case NodeKind::Math_Sqrt:
		case NodeKind::Math_Abs:
		case NodeKind::Math_Negate:
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Input, "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Float, PD::Output, "Out"));
			break;
		case NodeKind::Return_:
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Input, "In"));
			break;
		case NodeKind::LocalVar:
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Input, "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Output, "Out"));
			n.Pins.push_back(MakePin(NewID(), PT::Any, PD::Output, "Value"));
			break;
		case NodeKind::RawCode:
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Input, "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Output, "Out"));
			break;
		default:
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Input, "In"));
			n.Pins.push_back(MakePin(NewID(), PT::Exec, PD::Output, "Out"));
			break;
	}

	Nodes.push_back(std::move(n));
}

// ---------------------------------------------------------------------------
// Link factory
// ---------------------------------------------------------------------------

void NodeGraphCanvas::AddExecLink(int srcNodeID, int srcPinID, int dstNodeID, int dstPinID)
{
	Links.erase(std::remove_if(Links.begin(), Links.end(),
					[dstPinID](const NodeLink& l)
	{
		return l.DstPinID == dstPinID;
	}),
		Links.end());
	NodeLink link;
	link.ID        = NewID();
	link.SrcNodeID = srcNodeID;
	link.SrcPinID  = srcPinID;
	link.DstNodeID = dstNodeID;
	link.DstPinID  = dstPinID;
	Links.push_back(link);
}

void NodeGraphCanvas::AddDataLink(int srcNodeID, int srcPinID, int dstNodeID, int dstPinID)
{
	Links.erase(std::remove_if(Links.begin(), Links.end(),
					[dstPinID](const NodeLink& l)
	{
		return l.DstPinID == dstPinID;
	}),
		Links.end());
	NodeLink link;
	link.ID        = NewID();
	link.SrcNodeID = srcNodeID;
	link.SrcPinID  = srcPinID;
	link.DstNodeID = dstNodeID;
	link.DstPinID  = dstPinID;
	Links.push_back(link);
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

static float CalcPayloadH(const NodeGraphCanvas::ScriptNode& node)
{
	if (node.Payload.empty()) return 0.0f;
	if (node.Kind == NodeGraphCanvas::NodeKind::RawCode)
	{
		int lines = 1;
		for (char c : node.Payload)
			if (c == '\n') ++lines;
		return PinRowH * static_cast<float>(std::min(lines, 4));
	}
	return PinRowH;
}

void NodeGraphCanvas::DrawGrid(ImDrawList* dl, float canvasX, float canvasY,
	float canvasW, float canvasH) const
{
	float startX = std::fmod(CanvasScrollX, GridStep);
	float startY = std::fmod(CanvasScrollY, GridStep);

	for (float x = startX; x < canvasW; x += GridStep)
	{
		bool major = std::fmod(x - startX, GridStep * 4.0f) < 0.5f;
		dl->AddLine({ canvasX + x, canvasY }, { canvasX + x, canvasY + canvasH },
			major ? ColGridMajor : ColGrid, 1.0f);
	}
	for (float y = startY; y < canvasH; y += GridStep)
	{
		bool major = std::fmod(y - startY, GridStep * 4.0f) < 0.5f;
		dl->AddLine({ canvasX, canvasY + y }, { canvasX + canvasW, canvasY + y },
			major ? ColGridMajor : ColGrid, 1.0f);
	}
}

void NodeGraphCanvas::GetPinScreenPos(const ScriptNode& node, const Pin& pin,
	float canvasX, float canvasY,
	float& outX, float& outY) const
{
	float nodeScreenX = canvasX + node.PosX + CanvasScrollX;
	float nodeScreenY = canvasY + node.PosY + CanvasScrollY;

	bool isOutput = (pin.Dir == PinDir::Output);
	int row       = 0;
	for (const auto& p : node.Pins)
	{
		if (p.ID == pin.ID) break;
		if ((p.Dir == PinDir::Output) == isOutput) row++;
	}

	float payH = CalcPayloadH(node);
	float pinY = nodeScreenY + NodeTitleH + payH + NodePadY + PinRowH * static_cast<float>(row) + PinRowH * 0.5f;
	float pinX = isOutput ? (nodeScreenX + NodeWidth) : nodeScreenX;

	outX = pinX;
	outY = pinY;
}

bool NodeGraphCanvas::HitTestPin(float sx, float sy, float canvasX, float canvasY,
	int& outNodeID, int& outPinID, bool& outIsOutput) const
{
	for (const auto& node : Nodes)
	{
		for (const auto& pin : node.Pins)
		{
			float px, py;
			GetPinScreenPos(node, pin, canvasX, canvasY, px, py);
			float dx = sx - px, dy = sy - py;
			if (dx * dx + dy * dy <= PinHitRadius * PinHitRadius)
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

float NodeGraphCanvas::DrawNode(ImDrawList* dl, ScriptNode& node,
	float canvasX, float canvasY)
{
	float sx = canvasX + node.PosX + CanvasScrollX;
	float sy = canvasY + node.PosY + CanvasScrollY;

	int numIn = 0, numOut = 0;
	for (const auto& p : node.Pins)
	{
		if (p.Dir == PinDir::Input)
			numIn++;
		else
			numOut++;
	}
	int rows       = std::max(numIn, numOut);
	float payloadH = CalcPayloadH(node);
	float bodyH    = NodeTitleH + payloadH + NodePadY + PinRowH * static_cast<float>(std::max(rows, 1)) + NodePadY;
	node.BodyH     = bodyH;

	bool selected = (node.ID == SelectedNodeID);

	dl->AddRectFilled({ sx + 4, sy + 4 }, { sx + NodeWidth + 4, sy + bodyH + 4 }, IM_COL32(0, 0, 0, 80), 6.0f);
	dl->AddRectFilled({ sx, sy }, { sx + NodeWidth, sy + bodyH }, ColNodeBg, 6.0f);
	dl->AddRectFilled({ sx, sy }, { sx + NodeWidth, sy + NodeTitleH }, NodeKindTitleColor(node.Kind), 6.0f);
	dl->AddRectFilled({ sx, sy + NodeTitleH - 6.0f }, { sx + NodeWidth, sy + NodeTitleH }, NodeKindTitleColor(node.Kind), 0.0f);
	dl->AddRect({ sx, sy }, { sx + NodeWidth, sy + bodyH },
		selected ? ColSelected : ColNodeBorder, 6.0f, 0, selected ? 2.0f : 1.0f);
	dl->AddText({ sx + 8.0f, sy + 4.0f }, IM_COL32(255, 255, 255, 230), node.Title);

	if (!node.Payload.empty())
	{
		float payY = sy + NodeTitleH + 2.0f;
		float payX = sx + 6.0f;
		if (node.Kind == NodeKind::RawCode)
		{
			const char* p = node.Payload.c_str();
			for (int line = 0; line < 4 && *p; ++line)
			{
				const char* end = p;
				while (*end && *end != '\n')
					++end;
				dl->AddText({ payX, payY + PinRowH * static_cast<float>(line) },
					IM_COL32(190, 190, 140, 200), p, end);
				p = (*end == '\n') ? end + 1 : end;
			}
		}
		else
		{
			dl->AddText({ payX, payY }, IM_COL32(200, 175, 110, 210), node.Payload.c_str());
		}
	}

	int inRow = 0, outRow = 0;
	for (const auto& pin : node.Pins)
	{
		bool isOut = (pin.Dir == PinDir::Output);
		int row    = isOut ? outRow++ : inRow++;

		float pinY = sy + NodeTitleH + payloadH + NodePadY + PinRowH * static_cast<float>(row) + PinRowH * 0.5f;
		float pinX = isOut ? (sx + NodeWidth) : sx;

		uint32_t col = PinTypeColor(pin.Type);
		dl->AddCircleFilled({ pinX, pinY }, PinRadius, col);
		dl->AddCircle({ pinX, pinY }, PinRadius, IM_COL32(0, 0, 0, 160), 12, 1.5f);

		float labelX = isOut ? (pinX - PinRadius - 4.0f - ImGui::CalcTextSize(pin.Name).x)
							 : (pinX + PinRadius + 4.0f);
		if (pin.Name[0] != '\0')
			dl->AddText({ labelX, pinY - 7.0f }, IM_COL32(210, 210, 210, 200), pin.Name);
		if (pin.Type == PinType::Any && !pin.TypeName.empty())
		{
			float typeX = isOut ? (pinX - PinRadius - 4.0f - ImGui::CalcTextSize(pin.TypeName.c_str()).x)
								: (pinX + PinRadius + 4.0f);
			dl->AddText({ typeX, pinY + 3.0f }, IM_COL32(140, 140, 140, 170), pin.TypeName.c_str());
		}
	}

	return bodyH;
}

void NodeGraphCanvas::DrawLinks(ImDrawList* dl, float canvasX, float canvasY)
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

		dl->AddBezierCubic({ sx, sy }, { sx + BezierStrength, sy },
			{ ex - BezierStrength, ey }, { ex, ey },
			PinTypeColor(srcPin->Type), 2.0f);
	}
}

// ---------------------------------------------------------------------------
// Palette sidebar
// ---------------------------------------------------------------------------

void NodeGraphCanvas::DrawPalette(const char* uniqueID, float width, float height)
{
	char childID[128];
	snprintf(childID, sizeof(childID), "##Palette_%s", uniqueID);

	ImGui::BeginChild(childID, ImVec2(width, height), ImGuiChildFlags_None);
	ImGui::Text("Node Palette");

	char filterID[128];
	snprintf(filterID, sizeof(filterID), "##PalFilter_%s", uniqueID);
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputText(filterID, PaletteFilter, sizeof(PaletteFilter));
	ImGui::Separator();

	const char* currentCategory = nullptr;
	for (int i = 0; i < PaletteSize; i++)
	{
		const PaletteEntry& entry = Palette[i];

		if (PaletteFilter[0] != '\0')
		{
			std::string lower  = entry.Label;
			std::string filter = PaletteFilter;
			for (auto& c : lower)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			for (auto& c : filter)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			if (lower.find(filter) == std::string::npos) continue;
		}

		if (!currentCategory || strcmp(currentCategory, entry.Category) != 0)
		{
			currentCategory = entry.Category;
			ImGui::TextDisabled("%s", currentCategory);
		}

		ImGui::Indent(8.0f);
		char btnID[128];
		snprintf(btnID, sizeof(btnID), "%s##%s_%d", entry.Label, uniqueID, i);
		bool clicked = ImGui::Button(btnID, ImVec2(width - 24.0f, 0));
		ImGui::Unindent(8.0f);

		if (clicked)
		{
			float cx = -CanvasScrollX + 200.0f;
			float cy = -CanvasScrollY + 100.0f;
			if (entry.Kind <= NodeKind::Event_OnDestroy)
				AddEventNode(entry.Kind, cx, cy);
			else
				AddActionNode(entry.Kind, cx, cy);
		}
	}

	ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

void NodeGraphCanvas::DrawCanvas(const char* uniqueID, float canvasW, float canvasH)
{
	char btnID[128];
	snprintf(btnID, sizeof(btnID), "##Canvas_%s", uniqueID);

	ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton(btnID, ImVec2(canvasW, canvasH),
		ImGuiButtonFlags_MouseButtonLeft
			| ImGuiButtonFlags_MouseButtonRight
			| ImGuiButtonFlags_MouseButtonMiddle);

	bool canvasHovered = ImGui::IsItemHovered();
	bool canvasActive  = ImGui::IsItemActive();

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(canvasOrigin, { canvasOrigin.x + canvasW, canvasOrigin.y + canvasH }, true);

	dl->AddRectFilled(canvasOrigin, { canvasOrigin.x + canvasW, canvasOrigin.y + canvasH },
		IM_COL32(30, 30, 30, 255));

	DrawGrid(dl, canvasOrigin.x, canvasOrigin.y, canvasW, canvasH);
	DrawLinks(dl, canvasOrigin.x, canvasOrigin.y);

	ImGuiIO& io = ImGui::GetIO();

	if (canvasActive && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		float mx = io.MousePos.x;
		float my = io.MousePos.y;

		if (!bDraggingNode && !bCreatingLink)
		{
			int hitNode, hitPin;
			bool hitOut;
			if (HitTestPin(mx, my, canvasOrigin.x, canvasOrigin.y, hitNode, hitPin, hitOut))
			{
				bCreatingLink    = true;
				LinkSrcNodeID    = hitNode;
				LinkSrcPinID     = hitPin;
				bLinkSrcIsOutput = hitOut;
			}
			else
			{
				bool hitAny = false;
				for (auto& node : Nodes)
				{
					float nx = canvasOrigin.x + node.PosX + CanvasScrollX;
					float ny = canvasOrigin.y + node.PosY + CanvasScrollY;
					if (mx >= nx && mx <= nx + NodeWidth && my >= ny && my <= ny + node.BodyH)
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
			bool srcIsOut = false;
			float srcX = 0, srcY = 0;
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
			float ex = srcIsOut ? mx : srcX;
			float ey = srcIsOut ? my : srcY;
			float bx = srcIsOut ? srcX : mx;
			float by = srcIsOut ? srcY : my;
			dl->AddBezierCubic({ bx, by }, { bx + BezierStrength, by },
				{ ex - BezierStrength, ey }, { ex, ey }, ColLinkDraft, 1.5f);
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
				bool valid = (bLinkSrcIsOutput != dstIsOut) && (dstNode != LinkSrcNodeID);
				if (valid)
				{
					int srcNode = bLinkSrcIsOutput ? LinkSrcNodeID : dstNode;
					int srcPin  = bLinkSrcIsOutput ? LinkSrcPinID : dstPin;
					int dNode   = bLinkSrcIsOutput ? dstNode : LinkSrcNodeID;
					int dPin    = bLinkSrcIsOutput ? dstPin : LinkSrcPinID;

					Links.erase(std::remove_if(Links.begin(), Links.end(),
									[dPin](const NodeLink& l)
					{
						return l.DstPinID == dPin;
					}),
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

	// Pan: middle-drag OR right-drag OR scroll wheel
	if (canvasHovered)
	{
		if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 3.0f))
		{
			CanvasScrollX += io.MouseDelta.x;
			CanvasScrollY += io.MouseDelta.y;
		}
		if (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f)
		{
			CanvasScrollX += io.MouseWheelH * 24.0f;
			CanvasScrollY += io.MouseWheel * 24.0f;
		}
	}

	// Right-click context menu (only on a clean click, not after a drag)
	char ctxID[128];
	snprintf(ctxID, sizeof(ctxID), "##NodeCtxMenu_%s", uniqueID);

	if (canvasHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)
		&& !ImGui::IsMouseDragging(ImGuiMouseButton_Right, 3.0f))
	{
		float mx      = io.MousePos.x - canvasOrigin.x;
		float my      = io.MousePos.y - canvasOrigin.y;
		bool overNode = false;
		for (const auto& node : Nodes)
		{
			float nx = node.PosX + CanvasScrollX;
			float ny = node.PosY + CanvasScrollY;
			if (mx >= nx && mx <= nx + NodeWidth && my >= ny && my <= ny + node.BodyH)
			{
				overNode = true;
				break;
			}
		}
		if (!overNode)
		{
			ContextMenuCanvasX = mx - CanvasScrollX;
			ContextMenuCanvasY = my - CanvasScrollY;
			ImGui::OpenPopup(ctxID);
		}
	}

	// Delete selected node
	if (SelectedNodeID != -1 && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
	{
		Links.erase(std::remove_if(Links.begin(), Links.end(),
						[this](const NodeLink& l)
		{
			return l.SrcNodeID == SelectedNodeID || l.DstNodeID == SelectedNodeID;
		}),
			Links.end());
		Nodes.erase(std::remove_if(Nodes.begin(), Nodes.end(),
						[this](const ScriptNode& n)
		{
			return n.ID == SelectedNodeID;
		}),
			Nodes.end());
		SelectedNodeID = -1;
	}

	if (ImGui::BeginPopup(ctxID))
	{
		ImGui::Text("Add Node");
		ImGui::Separator();
		const char* lastCat = nullptr;
		for (int i = 0; i < PaletteSize; i++)
		{
			const PaletteEntry& entry = Palette[i];
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

	for (auto& node : Nodes)
		DrawNode(dl, node, canvasOrigin.x, canvasOrigin.y);

	dl->PopClipRect();
}
