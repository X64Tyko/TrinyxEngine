#pragma once
#if !defined(TNX_ENABLE_EDITOR)
#error "NodeScriptPanel.h requires TNX_ENABLE_EDITOR"
#endif

#include "EditorPanel.h"
#include <cstdint>
#include <string>
#include <vector>

// Forward declaration — avoids including imgui.h in the header.
struct ImDrawList;

/// NodeScriptPanel — Blueprint-like visual scripting panel for Trinyx.
///
/// Authors assemble logic from a node palette on a pannable canvas. On save,
/// the panel generates a native C++ header (using TNX_TEMPORAL_FIELDS /
/// TNX_VOLATILE_FIELDS macros and correct FieldProxy update bodies) that can
/// be hot-reloaded by the engine without touching hand-written code.
///
/// Execution model:
///   - Event nodes are execution roots (OnPrePhysics, OnPostPhysics,
///     OnUpdate, OnSpawn, OnDestroy).
///   - Branch nodes are forbidden in pre/post-physics graphs and rejected at
///     code-gen time with a clear error message (determinism constraint).
///   - All other nodes are pure data transforms — their output is inlined
///     into the generated update function body.
///
/// Node graph is stored entirely in CPU-side vectors; no GPU resources are
/// needed. Drawing uses ImGui's DrawList API (AddRectFilled / AddBezierCubic).
class NodeScriptPanel : public EditorPanel
{
public:
	NodeScriptPanel();
	~NodeScriptPanel() override = default;

	void Draw(EditorState& state) override;

	// -------------------------------------------------------------------------
	// Data model — public so file-scope palette tables in the .cpp can use them.
	// -------------------------------------------------------------------------

	enum class PinType : uint8_t { Exec, Float, Int, Bool, Vec3 };
	enum class PinDir  : uint8_t { Input, Output };

	struct Pin
	{
		int     ID   = 0;
		PinType Type = PinType::Exec;
		PinDir  Dir  = PinDir::Input;
		char    Name[32] = {};
	};

	enum class NodeKind : uint8_t
	{
		// Event roots
		Event_OnPrePhysics,
		Event_OnPostPhysics,
		Event_OnUpdate,
		Event_OnSpawn,
		Event_OnDestroy,

		// Flow control
		Sequence,
		Branch,      // forbidden in pre/post-physics

		// Property access
		GetProperty,
		SetProperty,

		// Math
		Math_Add,
		Math_Subtract,
		Math_Multiply,
		Math_Divide,
		Math_Clamp,
		Math_Lerp,

		// Vector
		Vec3_Make,
		Vec3_Break,
		Vec3_Length,
		Vec3_Normalize,
		Vec3_Scale,

		// Entity helpers
		GetPosition,
		SetPosition,
		GetVelocity,
		SetVelocity,
		ApplyImpulse,
	};

	struct ScriptNode
	{
		int         ID       = 0;
		NodeKind    Kind     = NodeKind::Event_OnUpdate;
		float       PosX    = 100.0f;
		float       PosY    = 100.0f;
		float       BodyH   = 40.0f; // Cached height from last DrawNode call — used for hit-testing.
		char        Title[64] = {};
		std::vector<Pin> Pins;

		bool IsEventNode() const
		{
			return Kind == NodeKind::Event_OnPrePhysics
				|| Kind == NodeKind::Event_OnPostPhysics
				|| Kind == NodeKind::Event_OnUpdate
				|| Kind == NodeKind::Event_OnSpawn
				|| Kind == NodeKind::Event_OnDestroy;
		}
	};

	struct NodeLink
	{
		int ID         = 0;
		int SrcNodeID  = 0;
		int SrcPinID   = 0;
		int DstNodeID  = 0;
		int DstPinID   = 0;
	};

	// -------------------------------------------------------------------------
	// Graph state
	// -------------------------------------------------------------------------

private:
	std::vector<ScriptNode> Nodes;
	std::vector<NodeLink>   Links;
	int NextID = 1;

	// -------------------------------------------------------------------------
	// Canvas interaction
	// -------------------------------------------------------------------------

	float CanvasScrollX = 0.0f;
	float CanvasScrollY = 0.0f;
	bool  bPanning      = false;

	int   SelectedNodeID = -1;
	bool  bDraggingNode  = false;
	float DragOffsetX    = 0.0f;
	float DragOffsetY    = 0.0f;

	// In-progress link drag
	bool bCreatingLink    = false;
	int  LinkSrcNodeID    = -1;
	int  LinkSrcPinID     = -1;
	bool bLinkSrcIsOutput = false;

	// Context menu
	bool  bShowContextMenu     = false;
	float ContextMenuCanvasX   = 0.0f;
	float ContextMenuCanvasY   = 0.0f;

	// Palette filter
	char PaletteFilter[64] = {};

	// -------------------------------------------------------------------------
	// Code-gen / export state
	// -------------------------------------------------------------------------

	char TargetEntityName[64] = "EMyEntity";
	char OutputPath[512]      = {};
	std::string GeneratedCode;
	bool bShowCodePreview     = false;
	char StatusMsg[1024]      = {};
	bool bStatusError         = false;

	// -------------------------------------------------------------------------
	// Private methods
	// -------------------------------------------------------------------------

	/// Add a fresh event node at the given canvas position.
	void AddEventNode(NodeKind kind, float cx, float cy);

	/// Add a fresh action/math/entity node at the given canvas position.
	void AddActionNode(NodeKind kind, float cx, float cy);

	/// Draw the background grid for the canvas.
	void DrawGrid(ImDrawList* dl, float canvasX, float canvasY,
				  float canvasW, float canvasH) const;

	/// Draw a single node. Returns node body height.
	float DrawNode(ImDrawList* dl, ScriptNode& node,
				   float canvasX, float canvasY);

	/// Draw all links (bezier curves) between connected pins.
	void DrawLinks(ImDrawList* dl, float canvasX, float canvasY);

	/// Compute the screen-space position of a pin on a node.
	/// pinSide: 0 = left (input), 1 = right (output).
	void GetPinScreenPos(const ScriptNode& node, const Pin& pin,
						 float canvasX, float canvasY,
						 float& outX, float& outY) const;

	/// Resolve a node/pin pair from a screen-space hit test.
	/// Returns true if a pin was found within the hit radius.
	bool HitTestPin(float sx, float sy, float canvasX, float canvasY,
					int& outNodeID, int& outPinID, bool& outIsOutput) const;

	/// Return the ImGui colour associated with a pin type.
	static uint32_t PinTypeColor(PinType t);

	/// Return the display title for a NodeKind.
	static const char* NodeKindTitle(NodeKind k);

	/// Topological code generation: walks from all event roots and emits C++.
	void GenerateCode();

	/// Write GeneratedCode to OutputPath.
	bool ExportToFile();

	/// Unique ID allocator — never returns the same value twice per session.
	int NewID() { return NextID++; }

	/// Lookup helpers.
	ScriptNode*       FindNode(int id);
	const ScriptNode* FindNode(int id) const;
	const Pin*        FindPin(int nodeID, int pinID) const;
};
