#include "TrinyxParser.h"
#if !defined(TNX_ENABLE_EDITOR)
#error "TrinyxParser.cpp requires TNX_ENABLE_EDITOR"
#endif

#include "NodeGraphCanvas.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

static std::string Trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	size_t b = s.find_last_not_of(" \t\r\n");
	return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

static bool IsNumericLiteral(const std::string& s)
{
	if (s.empty()) return false;
	size_t i = 0;
	if (s[i] == '-' || s[i] == '+') ++i;
	bool hasDigit = false;
	while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.'))
	{
		hasDigit = true;
		++i;
	}
	while (i < s.size() && (s[i] == 'f' || s[i] == 'u' || s[i] == 'L' || s[i] == 'l'))
		++i;
	return hasDigit && i == s.size();
}

// ---------------------------------------------------------------------------
// Expression simplifier
// ---------------------------------------------------------------------------

struct ExprResult
{
	int NodeID = -1;
	int PinID  = -1;
};

static ExprResult BuildExprNode(NodeGraphCanvas& g, const std::string& exprRaw, float x, float y)
{
	std::string expr = Trim(exprRaw);
	if (expr.empty()) return {};

	using NK = NodeGraphCanvas::NodeKind;

	for (const char* wrap : { "SimFloat(", "float(", "static_cast<float>(" })
	{
		size_t wlen = strlen(wrap);
		if (expr.size() > wlen + 1 && expr.substr(0, wlen) == wrap && expr.back() == ')')
		{
			expr = Trim(expr.substr(wlen, expr.size() - wlen - 1));
			break;
		}
	}

	// Strip trailing FieldProxy accessor calls (.Value(), .Get()) so the path
	// before the accessor is recognized as a GetProperty node.
	for (const char* suffix : { ".Value()", ".Get()" })
	{
		size_t slen = strlen(suffix);
		if (expr.size() > slen && expr.compare(expr.size() - slen, slen, suffix) == 0)
		{
			expr = expr.substr(0, expr.size() - slen);
			break;
		}
	}

	if (expr == "true" || expr == "false")
	{
		NodeGraphCanvas::ScriptNode n;
		n.ID      = g.NewID();
		n.Kind    = NK::Const_Bool;
		n.PosX    = x;
		n.PosY    = y;
		n.Payload = expr;
		snprintf(n.Title, sizeof(n.Title), "Bool");
		int outPin = g.NewID();
		n.Pins.push_back({ outPin, NodeGraphCanvas::PinType::Bool, NodeGraphCanvas::PinDir::Output, {} });
		snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "Value");
		int nodeID = n.ID;
		g.AddNode(std::move(n));
		return { nodeID, outPin };
	}

	if (expr == "dt" || expr == "deltaTime")
	{
		NodeGraphCanvas::ScriptNode n;
		n.ID   = g.NewID();
		n.Kind = NK::GetDeltaTime;
		n.PosX = x;
		n.PosY = y;
		snprintf(n.Title, sizeof(n.Title), "Get Dt");
		int outPin = g.NewID();
		n.Pins.push_back({ outPin, NodeGraphCanvas::PinType::Float, NodeGraphCanvas::PinDir::Output, {} });
		snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "dt");
		int nodeID = n.ID;
		g.AddNode(std::move(n));
		return { nodeID, outPin };
	}

	if (IsNumericLiteral(expr))
	{
		bool isInt = (expr.find('.') == std::string::npos && expr.back() != 'f');
		NodeGraphCanvas::ScriptNode n;
		n.ID      = g.NewID();
		n.Kind    = isInt ? NK::Const_Int : NK::Const_Float;
		n.PosX    = x;
		n.PosY    = y;
		n.Payload = expr;
		snprintf(n.Title, sizeof(n.Title), "%s", isInt ? "Int" : "Float");
		auto pt    = isInt ? NodeGraphCanvas::PinType::Int : NodeGraphCanvas::PinType::Float;
		int outPin = g.NewID();
		n.Pins.push_back({ outPin, pt, NodeGraphCanvas::PinDir::Output, {} });
		snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "Value");
		int nodeID = n.ID;
		g.AddNode(std::move(n));
		return { nodeID, outPin };
	}

	// Split on a binary comparison operator at depth 0: build a Compare node
	// wiring two recursive ExprResult nodes to its A and B inputs.
	{
		static const struct
		{
			const char* op;
			int len;
		} CmpOps[] = {
			{ "==", 2 }, { "!=", 2 }, { "<=", 2 }, { ">=", 2 }, { "<", 1 }, { ">", 1 }
		};
		int depth = 0;
		for (size_t i = 0; i < expr.size(); ++i)
		{
			char c = expr[i];
			if (c == '(' || c == '[')
			{
				++depth;
				continue;
			}
			if (c == ')' || c == ']')
			{
				--depth;
				continue;
			}
			if (depth != 0) continue;
			for (const auto& op : CmpOps)
			{
				if ((int)(expr.size() - i) >= op.len && expr.compare(i, (size_t)op.len, op.op) == 0)
				{
					std::string lhsStr = Trim(expr.substr(0, i));
					std::string rhsStr = Trim(expr.substr(i + (size_t)op.len));

					ExprResult lhs = BuildExprNode(g, lhsStr, x - 200.0f, y - 40.0f);
					ExprResult rhs = BuildExprNode(g, rhsStr, x - 200.0f, y + 40.0f);

					NodeGraphCanvas::ScriptNode cmp;
					cmp.ID      = g.NewID();
					cmp.Kind    = NK::Compare;
					cmp.PosX    = x;
					cmp.PosY    = y;
					cmp.Payload = op.op;
					snprintf(cmp.Title, sizeof(cmp.Title), "Compare");

					int pinA   = g.NewID();
					int pinB   = g.NewID();
					int pinOut = g.NewID();
					cmp.Pins.push_back({ pinA, NodeGraphCanvas::PinType::Float, NodeGraphCanvas::PinDir::Input, {} });
					snprintf(cmp.Pins.back().Name, sizeof(cmp.Pins.back().Name), "A");
					cmp.Pins.push_back({ pinB, NodeGraphCanvas::PinType::Float, NodeGraphCanvas::PinDir::Input, {} });
					snprintf(cmp.Pins.back().Name, sizeof(cmp.Pins.back().Name), "B");
					cmp.Pins.push_back({ pinOut, NodeGraphCanvas::PinType::Bool, NodeGraphCanvas::PinDir::Output, {} });
					snprintf(cmp.Pins.back().Name, sizeof(cmp.Pins.back().Name), "Result");

					int cmpID = cmp.ID;
					g.AddNode(std::move(cmp));
					if (lhs.NodeID != -1) g.AddDataLink(lhs.NodeID, lhs.PinID, cmpID, pinA);
					if (rhs.NodeID != -1) g.AddDataLink(rhs.NodeID, rhs.PinID, cmpID, pinB);

					return { cmpID, pinOut };
				}
			}
		}
	}

	{
		bool simple = !expr.empty();
		for (char c : expr)
			if (!std::isalnum((unsigned char)c) && c != '_' && c != '.' && c != '>')
			{
				simple = false;
				break;
			}
		if (simple)
		{
			NodeGraphCanvas::ScriptNode n;
			n.ID      = g.NewID();
			n.Kind    = NK::GetProperty;
			n.PosX    = x;
			n.PosY    = y;
			n.Payload = expr;
			snprintf(n.Title, sizeof(n.Title), "Get Property");
			int outPin = g.NewID();
			n.Pins.push_back({ outPin, NodeGraphCanvas::PinType::Float, NodeGraphCanvas::PinDir::Output, {} });
			snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "Value");
			int nodeID = n.ID;
			g.AddNode(std::move(n));
			return { nodeID, outPin };
		}
	}

	return {};
}

static NodeGraphCanvas::PinType MapCppTypeToPinType(const std::string& t)
{
	using PT = NodeGraphCanvas::PinType;
	if (t == "float" || t == "double" || t == "SimFloat" || t == "Fixed32")
		return PT::Float;
	if (t == "int" || t == "int32_t" || t == "uint32_t" || t == "int64_t" || t == "uint64_t" || t == "uint8_t" || t == "int16_t" || t == "uint16_t" || t == "size_t" || t == "ptrdiff_t")
		return PT::Int;
	if (t == "bool")
		return PT::Bool;
	if (t == "Vector3" || t == "Vec3" || t == "glm_vec3")
		return PT::Vec3;
	return PT::Any;
}

// ---------------------------------------------------------------------------
// Graph builder — converts TrinyxParser::Stmt IR into NodeGraphCanvas nodes
// ---------------------------------------------------------------------------

struct BuildState
{
	float X            = 200.0f;
	float Y            = 400.0f;
	float DataY        = 200.0f;
	int PrevExecNodeID = -1;
	int PrevExecPinID  = -1;

	float NextX()
	{
		float r = X;
		X += 220.0f;
		return r;
	}
};

static void EmitStmts(NodeGraphCanvas& g, const std::vector<TrinyxParser::Stmt>& stmts, BuildState& bs);

static void EmitStmt(NodeGraphCanvas& g, const TrinyxParser::Stmt& s, BuildState& bs)
{
	using NK = NodeGraphCanvas::NodeKind;
	using SK = TrinyxParser::StmtKind;

	auto linkExec = [&](int nodeID, int inPinID)
	{
		if (bs.PrevExecNodeID != -1 && bs.PrevExecPinID != -1)
			g.AddExecLink(bs.PrevExecNodeID, bs.PrevExecPinID, nodeID, inPinID);
	};

	switch (s.Kind)
	{
		case SK::Decl:
		{
			NodeGraphCanvas::ScriptNode n;
			n.ID      = g.NewID();
			n.Kind    = NK::LocalVar;
			n.PosX    = bs.NextX();
			n.PosY    = bs.Y;
			n.Payload = Trim(s.Rhs);
			snprintf(n.Title, sizeof(n.Title), "%s", s.Lhs.empty() ? "local" : s.Lhs.c_str());

			int inPin  = g.NewID();
			int outPin = g.NewID();
			int valPin = g.NewID();
			n.Pins.push_back({ inPin, NodeGraphCanvas::PinType::Exec, NodeGraphCanvas::PinDir::Input, {} });
			snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "In");
			n.Pins.push_back({ outPin, NodeGraphCanvas::PinType::Exec, NodeGraphCanvas::PinDir::Output, {} });
			snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "Out");

			NodeGraphCanvas::PinType valType = MapCppTypeToPinType(s.Raw);
			n.Pins.push_back({ valPin, valType, NodeGraphCanvas::PinDir::Output, {} });
			snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "Value");
			if (valType == NodeGraphCanvas::PinType::Any)
				n.Pins.back().TypeName = s.Raw;

			int nodeID = n.ID;
			g.AddNode(std::move(n));
			linkExec(nodeID, inPin);

			bs.PrevExecNodeID = nodeID;
			bs.PrevExecPinID  = outPin;
			break;
		}

		case SK::Return:
		{
			NodeGraphCanvas::ScriptNode n;
			n.ID   = g.NewID();
			n.Kind = NK::Return_;
			n.PosX = bs.NextX();
			n.PosY = bs.Y;
			snprintf(n.Title, sizeof(n.Title), "Return");
			int inPin = g.NewID();
			n.Pins.push_back({ inPin, NodeGraphCanvas::PinType::Exec, NodeGraphCanvas::PinDir::Input, {} });
			snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "In");
			int nodeID = n.ID;
			g.AddNode(std::move(n));
			linkExec(nodeID, inPin);
			bs.PrevExecNodeID = -1;
			bs.PrevExecPinID  = -1;
			break;
		}

		case SK::If:
		{
			NodeGraphCanvas::ScriptNode branch;
			branch.ID   = g.NewID();
			branch.Kind = NK::Branch;
			branch.PosX = bs.NextX();
			branch.PosY = bs.Y;
			snprintf(branch.Title, sizeof(branch.Title), "Branch");

			int inPin    = g.NewID();
			int condPin  = g.NewID();
			int truePin  = g.NewID();
			int falsePin = g.NewID();
			branch.Pins.push_back({ inPin, NodeGraphCanvas::PinType::Exec, NodeGraphCanvas::PinDir::Input, {} });
			snprintf(branch.Pins.back().Name, sizeof(branch.Pins.back().Name), "In");
			branch.Pins.push_back({ condPin, NodeGraphCanvas::PinType::Bool, NodeGraphCanvas::PinDir::Input, {} });
			snprintf(branch.Pins.back().Name, sizeof(branch.Pins.back().Name), "Condition");
			branch.Pins.push_back({ truePin, NodeGraphCanvas::PinType::Exec, NodeGraphCanvas::PinDir::Output, {} });
			snprintf(branch.Pins.back().Name, sizeof(branch.Pins.back().Name), "True");
			branch.Pins.push_back({ falsePin, NodeGraphCanvas::PinType::Exec, NodeGraphCanvas::PinDir::Output, {} });
			snprintf(branch.Pins.back().Name, sizeof(branch.Pins.back().Name), "False");

			int branchID     = branch.ID;
			float branchPosX = branch.PosX;
			g.AddNode(std::move(branch));
			linkExec(branchID, inPin);

			if (!s.Cond.empty())
			{
				ExprResult cond = BuildExprNode(g, s.Cond, branchPosX - 160.0f, bs.DataY - 60.0f);
				if (cond.NodeID != -1)
				{
					g.AddDataLink(cond.NodeID, cond.PinID, branchID, condPin);
				}
				else
				{
					// Fallback: raw condition text wired to Condition pin
					NodeGraphCanvas::ScriptNode raw;
					raw.ID      = g.NewID();
					raw.Kind    = NK::RawCode;
					raw.PosX    = branchPosX - 160.0f;
					raw.PosY    = bs.DataY - 60.0f;
					raw.Payload = Trim(s.Cond);
					snprintf(raw.Title, sizeof(raw.Title), "Code");
					int rawOut = g.NewID();
					raw.Pins.push_back({ rawOut, NodeGraphCanvas::PinType::Bool, NodeGraphCanvas::PinDir::Output, {} });
					snprintf(raw.Pins.back().Name, sizeof(raw.Pins.back().Name), "Value");
					int rawID = raw.ID;
					g.AddNode(std::move(raw));
					g.AddDataLink(rawID, rawOut, branchID, condPin);
				}
			}

			BuildState trueBs;
			trueBs.X              = bs.X;
			trueBs.Y              = bs.Y + 140.0f;
			trueBs.DataY          = bs.Y + 60.0f;
			trueBs.PrevExecNodeID = branchID;
			trueBs.PrevExecPinID  = truePin;
			EmitStmts(g, s.Then, trueBs);

			if (!s.Else.empty())
			{
				BuildState elseBs;
				elseBs.X              = trueBs.X + 60.0f;
				elseBs.Y              = bs.Y + 280.0f;
				elseBs.DataY          = bs.Y + 200.0f;
				elseBs.PrevExecNodeID = branchID;
				elseBs.PrevExecPinID  = falsePin;
				EmitStmts(g, s.Else, elseBs);
			}

			bs.X              = std::max(trueBs.X, bs.X) + 20.0f;
			bs.PrevExecNodeID = -1;
			bs.PrevExecPinID  = -1;
			break;
		}

		case SK::Assign:
		{
			NodeGraphCanvas::ScriptNode n;
			n.ID      = g.NewID();
			n.Kind    = NK::SetProperty;
			n.PosX    = bs.NextX();
			n.PosY    = bs.Y;
			n.Payload = Trim(s.Lhs);
			snprintf(n.Title, sizeof(n.Title), "Set Property");

			int inPin  = g.NewID();
			int valPin = g.NewID();
			int outPin = g.NewID();
			n.Pins.push_back({ inPin, NodeGraphCanvas::PinType::Exec, NodeGraphCanvas::PinDir::Input, {} });
			snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "In");
			n.Pins.push_back({ valPin, NodeGraphCanvas::PinType::Float, NodeGraphCanvas::PinDir::Input, {} });
			snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "Value");
			n.Pins.push_back({ outPin, NodeGraphCanvas::PinType::Exec, NodeGraphCanvas::PinDir::Output, {} });
			snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "Out");

			int nodeID     = n.ID;
			float nodePosX = n.PosX;
			g.AddNode(std::move(n));
			linkExec(nodeID, inPin);

			ExprResult rhs = BuildExprNode(g, s.Rhs, nodePosX - 160.0f, bs.DataY);
			if (rhs.NodeID != -1)
			{
				g.AddDataLink(rhs.NodeID, rhs.PinID, nodeID, valPin);
				bs.DataY -= 60.0f;
			}

			bs.PrevExecNodeID = nodeID;
			bs.PrevExecPinID  = outPin;
			break;
		}

		default: // Call / Raw
		{
			std::string raw = Trim(s.Raw.empty() ? s.Rhs : s.Raw);
			if (raw.empty()) break;

			NodeGraphCanvas::ScriptNode n;
			n.ID      = g.NewID();
			n.Kind    = NK::RawCode;
			n.PosX    = bs.NextX();
			n.PosY    = bs.Y;
			n.Payload = raw;
			snprintf(n.Title, sizeof(n.Title), "Code");

			int inPin  = g.NewID();
			int outPin = g.NewID();
			n.Pins.push_back({ inPin, NodeGraphCanvas::PinType::Exec, NodeGraphCanvas::PinDir::Input, {} });
			snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "In");
			n.Pins.push_back({ outPin, NodeGraphCanvas::PinType::Exec, NodeGraphCanvas::PinDir::Output, {} });
			snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "Out");

			int nodeID = n.ID;
			g.AddNode(std::move(n));
			linkExec(nodeID, inPin);

			bs.PrevExecNodeID = nodeID;
			bs.PrevExecPinID  = outPin;
			break;
		}
	}
}

static void EmitStmts(NodeGraphCanvas& g, const std::vector<TrinyxParser::Stmt>& stmts, BuildState& bs)
{
	for (const auto& s : stmts)
		EmitStmt(g, s, bs);
}

// ---------------------------------------------------------------------------
// Public bridge — called from ConstructEditorWindow
// ---------------------------------------------------------------------------

void BuildGraphFromParsedMethod(const TrinyxParser::ParsedMethod& method,
	NodeGraphCanvas& g,
	int entryNodeID,
	int entryExecPinID)
{
	if (!method.Found) return;

	BuildState bs;
	bs.X              = 340.0f;
	bs.Y              = 120.0f;
	bs.DataY          = 40.0f;
	bs.PrevExecNodeID = entryNodeID;
	bs.PrevExecPinID  = entryExecPinID;

	EmitStmts(g, method.Body, bs);
}
