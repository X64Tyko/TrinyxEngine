#pragma once
#if !defined(TNX_ENABLE_EDITOR)
#error "NodeGraphCanvas.h requires TNX_ENABLE_EDITOR"
#endif

#include <cstdint>
#include <string>
#include <vector>

struct ImDrawList;

/// Self-contained node graph canvas — state + interaction + drawing.
/// Designed to be embedded by value (one per open document tab) in any editor window.
/// Call DrawPalette() then DrawCanvas() each frame, or just DrawCanvas() if the
/// palette is handled separately.
class NodeGraphCanvas
{
public:
	// -------------------------------------------------------------------------
	// Data model
	// -------------------------------------------------------------------------

	enum class PinType : uint8_t
	{
		Exec,
		Float,
		Int,
		Bool,
		Vec3,
		Any
	};
	enum class PinDir : uint8_t
	{
		Input,
		Output
	};

	struct Pin
	{
		int ID        = 0;
		PinType Type  = PinType::Exec;
		PinDir Dir    = PinDir::Input;
		char Name[32] = {};
		std::string TypeName; // non-empty when Type == Any; holds the raw C++ type string
	};

	enum class NodeKind : uint8_t
	{
		Event_OnPrePhysics,
		Event_OnPhysicsStep,
		Event_OnPostPhysics,
		Event_OnUpdate,
		Event_OnSpawn,
		Event_OnDestroy,
		Sequence,
		Branch,
		GetProperty,
		SetProperty,
		Math_Add,
		Math_Subtract,
		Math_Multiply,
		Math_Divide,
		Math_Clamp,
		Math_Lerp,
		Vec3_Make,
		Vec3_Break,
		Vec3_Length,
		Vec3_Normalize,
		Vec3_Scale,
		GetPosition,
		SetPosition,
		GetVelocity,
		SetVelocity,
		ApplyImpulse,

		// Literals / values (data-only, no exec pins)
		Const_Float,
		Const_Int,
		Const_Bool,
		GetDeltaTime,

		// Logic
		Compare, // Payload = operator string: "<" ">" "<=" ">=" "==" "!="
		LogicalAnd,
		LogicalOr,
		LogicalNot,

		// Input
		CheckAction, // Payload = action name

		// Additional math
		Math_Sqrt,
		Math_Abs,
		Math_Negate,

		// Flow
		Return_,

		// Local variable — Payload = init expression, Title = variable name
		// Has Exec in/out + one data output carrying the value
		LocalVar,

		// User-defined function entry (TNXFUNC-tagged methods) — Title = display name
		FuncEntry,

		// Parser-generated fallback — stores raw C++ text in Payload
		RawCode,
	};

	struct ScriptNode
	{
		int ID         = 0;
		NodeKind Kind  = NodeKind::Event_OnUpdate;
		float PosX     = 100.0f;
		float PosY     = 100.0f;
		float BodyH    = 40.0f;
		char Title[64] = {};
		std::string Payload; // property path, literal value, operator, raw code, etc.
		std::vector<Pin> Pins;

		bool IsEventNode() const
		{
			return Kind == NodeKind::Event_OnPrePhysics
				   || Kind == NodeKind::Event_OnPhysicsStep
				   || Kind == NodeKind::Event_OnPostPhysics
				   || Kind == NodeKind::Event_OnUpdate
				   || Kind == NodeKind::Event_OnSpawn
				   || Kind == NodeKind::Event_OnDestroy;
		}
	};

	struct NodeLink
	{
		int ID        = 0;
		int SrcNodeID = 0;
		int SrcPinID  = 0;
		int DstNodeID = 0;
		int DstPinID  = 0;
	};

	// -------------------------------------------------------------------------
	// Graph state — public for code-gen access by parent panels
	// -------------------------------------------------------------------------

	std::vector<ScriptNode> Nodes;
	std::vector<NodeLink> Links;

	// -------------------------------------------------------------------------
	// Construction
	// -------------------------------------------------------------------------

	NodeGraphCanvas();

	/// Seed the graph with a default OnUpdate event node.
	void AddDefaultEventNode();

	/// Remove all nodes and links, reset view.
	void Clear();

	// -------------------------------------------------------------------------
	// Drawing — uniqueID prevents ImGui widget collisions when multiple canvases
	// exist simultaneously (one per tab). Must be unique within the parent window.
	// -------------------------------------------------------------------------

	/// Draw the node palette sidebar (category headers + clickable node buttons).
	/// width/height = the region available for the palette child window.
	void DrawPalette(const char* uniqueID, float width, float height);

	/// Draw the canvas background, grid, links, nodes, and handle all interaction.
	/// canvasW/canvasH = the available region for the canvas child window.
	void DrawCanvas(const char* uniqueID, float canvasW, float canvasH);

	// -------------------------------------------------------------------------
	// Node factory
	// -------------------------------------------------------------------------

	void AddEventNode(NodeKind kind, float cx, float cy);
	void AddFuncEntryNode(const char* displayName, float cx, float cy);
	void AddActionNode(NodeKind kind, float cx, float cy);

	/// Add a pre-built node directly (used by the body parser).
	void AddNode(ScriptNode node) { Nodes.push_back(std::move(node)); }

	/// Wire an exec-out pin to an exec-in pin.
	void AddExecLink(int srcNodeID, int srcPinID, int dstNodeID, int dstPinID);
	/// Wire any two data pins.
	void AddDataLink(int srcNodeID, int srcPinID, int dstNodeID, int dstPinID);

	// -------------------------------------------------------------------------
	// Utilities
	// -------------------------------------------------------------------------

	int NewID() { return NextID++; }

	ScriptNode* FindNode(int id);
	const ScriptNode* FindNode(int id) const;
	const Pin* FindPin(int nodeID, int pinID) const;

	static uint32_t PinTypeColor(PinType t);
	static const char* NodeKindTitle(NodeKind k);

private:
	// -------------------------------------------------------------------------
	// Canvas interaction state
	// -------------------------------------------------------------------------

	int NextID          = 1;
	float CanvasScrollX = 0.0f;
	float CanvasScrollY = 0.0f;
	int SelectedNodeID  = -1;
	bool bDraggingNode  = false;
	float DragOffsetX   = 0.0f;
	float DragOffsetY   = 0.0f;

	bool bCreatingLink    = false;
	int LinkSrcNodeID     = -1;
	int LinkSrcPinID      = -1;
	bool bLinkSrcIsOutput = false;

	float ContextMenuCanvasX = 0.0f;
	float ContextMenuCanvasY = 0.0f;

	char PaletteFilter[64] = {};

	// -------------------------------------------------------------------------
	// Private drawing helpers
	// -------------------------------------------------------------------------

	void DrawGrid(ImDrawList* dl, float canvasX, float canvasY, float canvasW, float canvasH) const;
	float DrawNode(ImDrawList* dl, ScriptNode& node, float canvasX, float canvasY);
	void DrawLinks(ImDrawList* dl, float canvasX, float canvasY);

	void GetPinScreenPos(const ScriptNode& node, const Pin& pin,
		float canvasX, float canvasY,
		float& outX, float& outY) const;

	bool HitTestPin(float sx, float sy, float canvasX, float canvasY,
		int& outNodeID, int& outPinID, bool& outIsOutput) const;
};
