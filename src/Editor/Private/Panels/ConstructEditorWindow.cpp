#include "Panels/ConstructEditorWindow.h"
#if !defined(TNX_ENABLE_EDITOR)
#error "ConstructEditorWindow.cpp requires TNX_ENABLE_EDITOR"
#endif

#include "EditorState.h"
#include "EngineConfig.h"
#include "TnxStyle.h"
#include "TnxWidgets.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

static const char* SlotNames[] = { "Pre-Physics", "Physics Step", "Post-Physics", "Scalar Update" };

// ---------------------------------------------------------------------------
// TrinyxParser integration — graph builder lives in src/Editor/Private/TrinyxParser.cpp
// ---------------------------------------------------------------------------

#include "TrinyxParser.h"

void BuildGraphFromParsedMethod(const TrinyxParser::ParsedMethod& method,
	NodeGraphCanvas& g,
	int entryNodeID,
	int entryExecPinID);

static NodeGraphCanvas::NodeKind LifecycleToEventKind(TrinyxParser::LifecycleEvent lc)
{
	using LE = TrinyxParser::LifecycleEvent;
	using NK = NodeGraphCanvas::NodeKind;
	switch (lc)
	{
		case LE::PrePhysics: return NK::Event_OnPrePhysics;
		case LE::PhysicsStep: return NK::Event_OnPhysicsStep;
		case LE::PostPhysics: return NK::Event_OnPostPhysics;
		case LE::ScalarUpdate: return NK::Event_OnUpdate;
		case LE::OnSpawn: return NK::Event_OnSpawn;
		case LE::OnDestroy: return NK::Event_OnDestroy;
		default: return NK::Event_OnUpdate;
	}
}

static int LifecycleToSlotIndex(TrinyxParser::LifecycleEvent lc)
{
	using LE = TrinyxParser::LifecycleEvent;
	using TS = ConstructDoc::TickSlot;
	switch (lc)
	{
		case LE::PrePhysics: return static_cast<int>(TS::PrePhysics);
		case LE::PhysicsStep: return static_cast<int>(TS::PhysicsStep);
		case LE::PostPhysics: return static_cast<int>(TS::PostPhysics);
		case LE::ScalarUpdate: return static_cast<int>(TS::ScalarUpdate);
		default: return -1;
	}
}

#if 0
namespace CppGraph_REMOVED_SENTINEL_DO_NOT_USE
{

// --- Tokenizer ---------------------------------------------------------------

enum class TK : uint8_t
{
    Ident, Number, Str,
    LParen, RParen, LBrace, RBrace, Semi, Comma, Dot,
    Assign, CompEq, CompNe, CompLt, CompLe, CompGt, CompGe,
    AmpAmp, PipePipe, Bang,
    Plus, Minus, Star, Slash, Percent, Arrow,
    Colon, ColonColon, Eof, Other
};

struct Token { TK Kind; std::string Text; };

static std::vector<Token> Tokenize(const std::string& src)
{
    std::vector<Token> toks;
    size_t i = 0, n = src.size();
    while (i < n)
    {
        // Skip whitespace
        if (std::isspace((unsigned char)src[i])) { ++i; continue; }
        // Line comment
        if (i + 1 < n && src[i] == '/' && src[i+1] == '/')
        {
            while (i < n && src[i] != '\n') ++i;
            continue;
        }
        // Block comment
        if (i + 1 < n && src[i] == '/' && src[i+1] == '*')
        {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i+1] == '/')) ++i;
            if (i + 1 < n) i += 2;
            continue;
        }
        // Identifier / keyword
        if (std::isalpha((unsigned char)src[i]) || src[i] == '_')
        {
            size_t s = i;
            while (i < n && (std::isalnum((unsigned char)src[i]) || src[i] == '_')) ++i;
            toks.push_back({ TK::Ident, src.substr(s, i - s) });
            continue;
        }
        // Number literal
        if (std::isdigit((unsigned char)src[i]) || (src[i] == '.' && i+1 < n && std::isdigit((unsigned char)src[i+1])))
        {
            size_t s = i;
            while (i < n && (std::isdigit((unsigned char)src[i]) || src[i] == '.' || src[i] == 'f' || src[i] == 'u' || src[i] == 'L')) ++i;
            toks.push_back({ TK::Number, src.substr(s, i - s) });
            continue;
        }
        // String literal (skip contents)
        if (src[i] == '"')
        {
            size_t s = i++;
            while (i < n && src[i] != '"') { if (src[i] == '\\') ++i; ++i; }
            if (i < n) ++i;
            toks.push_back({ TK::Str, src.substr(s, i - s) });
            continue;
        }
        // Two-char operators
        if (i + 1 < n)
        {
            char a = src[i], b = src[i+1];
            if (a == '=' && b == '=') { toks.push_back({ TK::CompEq, "==" }); i += 2; continue; }
            if (a == '!' && b == '=') { toks.push_back({ TK::CompNe, "!=" }); i += 2; continue; }
            if (a == '<' && b == '=') { toks.push_back({ TK::CompLe, "<=" }); i += 2; continue; }
            if (a == '>' && b == '=') { toks.push_back({ TK::CompGe, ">=" }); i += 2; continue; }
            if (a == '&' && b == '&') { toks.push_back({ TK::AmpAmp, "&&" }); i += 2; continue; }
            if (a == '|' && b == '|') { toks.push_back({ TK::PipePipe, "||" }); i += 2; continue; }
            if (a == '-' && b == '>') { toks.push_back({ TK::Arrow, "->" }); i += 2; continue; }
            if (a == ':' && b == ':') { toks.push_back({ TK::ColonColon, "::" }); i += 2; continue; }
        }
        // Single-char
        TK kind = TK::Other;
        std::string text(1, src[i]);
        switch (src[i])
        {
            case '(': kind = TK::LParen;   break;
            case ')': kind = TK::RParen;   break;
            case '{': kind = TK::LBrace;   break;
            case '}': kind = TK::RBrace;   break;
            case ';': kind = TK::Semi;     break;
            case ',': kind = TK::Comma;    break;
            case '.': kind = TK::Dot;      break;
            case '=': kind = TK::Assign;   break;
            case '<': kind = TK::CompLt;   break;
            case '>': kind = TK::CompGt;   break;
            case '!': kind = TK::Bang;     break;
            case '+': kind = TK::Plus;     break;
            case '-': kind = TK::Minus;    break;
            case '*': kind = TK::Star;     break;
            case '/': kind = TK::Slash;    break;
            case '%': kind = TK::Percent;  break;
            case ':': kind = TK::Colon;    break;
            default:  break;
        }
        toks.push_back({ kind, text });
        ++i;
    }
    toks.push_back({ TK::Eof, "" });
    return toks;
}

// --- Token stream ------------------------------------------------------------

struct TStream
{
    const std::vector<Token>& T;
    int Pos = 0;

    const Token& Peek(int off = 0) const
    {
        int idx = Pos + off;
        if (idx >= static_cast<int>(T.size())) return T.back();
        return T[idx];
    }
    const Token& Consume() { return T[Pos < static_cast<int>(T.size()) ? Pos++ : Pos]; }
    bool At(TK k) const { return Peek().Kind == k; }
    bool AtIdent(const char* s) const { return At(TK::Ident) && Peek().Text == s; }

    // Consume tokens until one of the stop kinds, collecting raw text
    std::string CollectUntil(std::initializer_list<TK> stopKinds, int depth = 0)
    {
        std::string out;
        while (!At(TK::Eof))
        {
            TK k = Peek().Kind;
            if (k == TK::LParen || k == TK::LBrace) ++depth;
            if (k == TK::RParen || k == TK::RBrace) { if (depth <= 0) break; --depth; }
            for (TK sk : stopKinds)
                if (k == sk && depth == 0) { if (!out.empty() && out.back() == ' ') out.pop_back(); return out; }
            if (!out.empty()) out += ' ';
            out += Consume().Text;
        }
        return out;
    }

    // Skip balanced braces, return content between them
    std::string ConsumeBlock()
    {
        if (!At(TK::LBrace)) return {};
        Consume(); // '{'
        std::string out;
        int depth = 0;
        while (!At(TK::Eof))
        {
            if (At(TK::LBrace)) { ++depth; out += "{ "; Consume(); continue; }
            if (At(TK::RBrace))
            {
                if (depth == 0) { Consume(); break; }
                --depth; out += "} "; Consume(); continue;
            }
            out += Peek().Text + " ";
            Consume();
        }
        return out;
    }
};

// --- Statement IR ------------------------------------------------------------

enum class StmtKind : uint8_t { If, Return, Assign, Call, Raw, Decl };

struct Stmt
{
    StmtKind Kind = StmtKind::Raw;
    std::string Cond;       // If: condition text
    std::string Lhs;        // Assign: left-hand side member path
    std::string Rhs;        // Assign: right-hand side expression text
    std::string Raw;        // Raw/Call: verbatim text
    std::vector<Stmt> Then; // If: true branch
    std::vector<Stmt> Else; // If: else branch
};

static std::vector<Stmt> ParseBlock(TStream& ts);

static Stmt ParseStmt(TStream& ts)
{
    Stmt s;

    // return statement
    if (ts.AtIdent("return"))
    {
        ts.Consume();
        s.Kind = StmtKind::Return;
        s.Raw  = ts.CollectUntil({ TK::Semi });
        if (ts.At(TK::Semi)) ts.Consume();
        return s;
    }

    // if statement
    if (ts.AtIdent("if"))
    {
        ts.Consume();
        if (ts.At(TK::LParen)) ts.Consume();
        s.Cond = ts.CollectUntil({ TK::RParen }, 1);
        if (ts.At(TK::RParen)) ts.Consume();
        s.Kind = StmtKind::If;
        if (ts.At(TK::LBrace))
            s.Then = ParseBlock(ts);
        else
        {
            Stmt inner = ParseStmt(ts);
            s.Then.push_back(std::move(inner));
        }
        if (ts.AtIdent("else"))
        {
            ts.Consume();
            if (ts.At(TK::LBrace))
                s.Else = ParseBlock(ts);
            else
            {
                Stmt inner = ParseStmt(ts);
                s.Else.push_back(std::move(inner));
            }
        }
        return s;
    }

    // Skip for/while/do as raw
    if (ts.AtIdent("for") || ts.AtIdent("while") || ts.AtIdent("do"))
    {
        s.Kind = StmtKind::Raw;
        // Collect keyword + condition
        s.Raw = ts.Consume().Text; // for/while/do
        if (ts.At(TK::LParen))
        {
            ts.Consume();
            s.Raw += "(" + ts.CollectUntil({ TK::RParen }, 1) + ")";
            if (ts.At(TK::RParen)) ts.Consume();
        }
        // Consume body block or single statement and mark as raw
        if (ts.At(TK::LBrace)) s.Raw += " { ... }";
        ts.ConsumeBlock();
        return s;
    }

    // Skip type declarations (const / auto / type keyword followed by ident then maybe =)
    // Detect: optional "const" then ident ident [= ...] ;
    {
        int saved = ts.Pos;
        bool isDecl = false;
        if (ts.AtIdent("const")) ts.Consume();
        // If next two tokens are Ident Ident (type + varname), it's a decl
        if (ts.At(TK::Ident))
        {
            ts.Consume();
            // Could be template — skip < ... >
            if (ts.At(TK::CompLt))
            {
                ts.Consume();
                int depth = 1;
                while (!ts.At(TK::Eof) && depth > 0)
                {
                    if (ts.At(TK::CompLt)) ++depth;
                    if (ts.At(TK::CompGt)) --depth;
                    ts.Consume();
                }
            }
            if (ts.At(TK::Ident))
            {
                ts.Consume();
                // Could have [N] subscript
                if (ts.At(TK::Other) && ts.Peek().Text == "[") { ts.Consume(); ts.CollectUntil({ TK::Other }); ts.Consume(); }
                if (ts.At(TK::Assign) || ts.At(TK::Semi) || ts.At(TK::LBrace))
                    isDecl = true;
            }
        }
        ts.Pos = saved;
        if (isDecl)
        {
            s.Kind = StmtKind::Decl;
            if (ts.AtIdent("const")) ts.Consume();

            // Extract base type name (first identifier)
            if (ts.At(TK::Ident)) s.Raw = ts.Consume().Text; // Raw = C++ type name

            // Skip template arguments < ... >
            if (ts.At(TK::CompLt))
            {
                int depth = 1;
                ts.Consume();
                while (!ts.At(TK::Eof) && depth > 0)
                {
                    if (ts.At(TK::CompLt)) ++depth;
                    if (ts.At(TK::CompGt)) --depth;
                    ts.Consume();
                }
            }

            // Variable name
            if (ts.At(TK::Ident)) s.Lhs = ts.Consume().Text;

            // Skip optional array subscript
            if (ts.At(TK::Other) && ts.Peek().Text == "[")
            { ts.Consume(); ts.CollectUntil({ TK::Other }); ts.Consume(); }

            // Initializer
            if (ts.At(TK::Assign)) { ts.Consume(); s.Rhs = ts.CollectUntil({ TK::Semi }); }
            if (ts.At(TK::Semi)) ts.Consume();
            return s;
        }
    }

    // Try to detect assignment: memberPath = expr ;
    // Walk ident(.ident)* then check for '='
    {
        int saved = ts.Pos;
        std::string lhs;
        if (ts.At(TK::Ident))
        {
            lhs = ts.Consume().Text;
            while (ts.At(TK::Dot) || ts.At(TK::Arrow))
            {
                lhs += ts.Consume().Text;
                if (ts.At(TK::Ident)) lhs += ts.Consume().Text;
            }
            if (ts.At(TK::Assign))
            {
                ts.Consume(); // '='
                s.Kind = StmtKind::Assign;
                s.Lhs  = lhs;
                s.Rhs  = ts.CollectUntil({ TK::Semi });
                if (ts.At(TK::Semi)) ts.Consume();
                return s;
            }
        }
        ts.Pos = saved;
    }

    // Fallback — collect until semicolon as raw call
    s.Kind = StmtKind::Call;
    s.Raw  = ts.CollectUntil({ TK::Semi, TK::RBrace });
    if (ts.At(TK::Semi)) ts.Consume();
    return s;
}

static std::vector<Stmt> ParseBlock(TStream& ts)
{
    std::vector<Stmt> stmts;
    if (ts.At(TK::LBrace)) ts.Consume();
    while (!ts.At(TK::Eof) && !ts.At(TK::RBrace))
        stmts.push_back(ParseStmt(ts));
    if (ts.At(TK::RBrace)) ts.Consume();
    return stmts;
}

// --- Expression simplifier + node emitter ------------------------------------

struct ExprResult { int NodeID = -1; int PinID = -1; };

// Trim whitespace from both ends of a string
static std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

// Returns true if text is a numeric literal (integer or float)
static bool IsNumericLiteral(const std::string& s)
{
    if (s.empty()) return false;
    size_t i = 0;
    if (s[i] == '-' || s[i] == '+') ++i;
    bool hasDigit = false;
    while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.')) { hasDigit = true; ++i; }
    while (i < s.size() && (s[i] == 'f' || s[i] == 'u' || s[i] == 'L' || s[i] == 'l')) ++i;
    return hasDigit && i == s.size();
}

// Build a node for a simple expression. Returns the node+pin that produces the value.
// Returns {-1,-1} when the expr is too complex to map to a single node.
static ExprResult BuildExprNode(NodeGraphCanvas& g, const std::string& exprRaw, float x, float y)
{
    std::string expr = Trim(exprRaw);
    if (expr.empty()) return {};

    using NK = NodeGraphCanvas::NodeKind;

    // Strip SimFloat(...) / float(...) / static_cast<float>(...) wrappers
    for (const char* wrap : { "SimFloat(", "float(", "static_cast<float>(" })
    {
        size_t wlen = strlen(wrap);
        if (expr.size() > wlen + 1 && expr.substr(0, wlen) == wrap && expr.back() == ')')
        {
            expr = Trim(expr.substr(wlen, expr.size() - wlen - 1));
            break;
        }
    }

    // Boolean literals
    if (expr == "true" || expr == "false")
    {
        NodeGraphCanvas::ScriptNode n;
        n.ID      = g.NewID();
        n.Kind    = NK::Const_Bool;
        n.PosX    = x; n.PosY = y;
        n.Payload = expr;
        snprintf(n.Title, sizeof(n.Title), "Bool");
        int outPin = g.NewID();
        n.Pins.push_back({ outPin, NodeGraphCanvas::PinType::Bool, NodeGraphCanvas::PinDir::Output, {} });
        snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "Value");
        int nodeID = n.ID;
        g.AddNode(std::move(n));
        return { nodeID, outPin };
    }

    // dt / deltaTime
    if (expr == "dt" || expr == "deltaTime")
    {
        NodeGraphCanvas::ScriptNode n;
        n.ID   = g.NewID();
        n.Kind = NK::GetDeltaTime;
        n.PosX = x; n.PosY = y;
        snprintf(n.Title, sizeof(n.Title), "Get Dt");
        int outPin = g.NewID();
        n.Pins.push_back({ outPin, NodeGraphCanvas::PinType::Float, NodeGraphCanvas::PinDir::Output, {} });
        snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "dt");
        int nodeID = n.ID;
        g.AddNode(std::move(n));
        return { nodeID, outPin };
    }

    // Numeric literal → Const_Float or Const_Int
    if (IsNumericLiteral(expr))
    {
        bool isInt = (expr.find('.') == std::string::npos && expr.back() != 'f');
        NodeGraphCanvas::ScriptNode n;
        n.ID      = g.NewID();
        n.Kind    = isInt ? NK::Const_Int : NK::Const_Float;
        n.PosX    = x; n.PosY = y;
        n.Payload = expr;
        snprintf(n.Title, sizeof(n.Title), "%s", isInt ? "Int" : "Float");
        auto pt   = isInt ? NodeGraphCanvas::PinType::Int : NodeGraphCanvas::PinType::Float;
        int outPin = g.NewID();
        n.Pins.push_back({ outPin, pt, NodeGraphCanvas::PinDir::Output, {} });
        snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "Value");
        int nodeID = n.ID;
        g.AddNode(std::move(n));
        return { nodeID, outPin };
    }

    // Simple member path (letters, digits, _, ., ->) with no parens/spaces → GetProperty
    {
        bool simple = !expr.empty();
        for (char c : expr)
            if (!std::isalnum((unsigned char)c) && c != '_' && c != '.' && c != '>') { simple = false; break; }
        // Reject if it contains -> but arrow still maps to property read
        if (simple)
        {
            NodeGraphCanvas::ScriptNode n;
            n.ID      = g.NewID();
            n.Kind    = NK::GetProperty;
            n.PosX    = x; n.PosY = y;
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

// Maps a C++ type name to the closest PinType; returns Any for complex/unknown types.
static NodeGraphCanvas::PinType MapCppTypeToPinType(const std::string& t)
{
    using PT = NodeGraphCanvas::PinType;
    if (t == "float" || t == "double" || t == "SimFloat" || t == "Fixed32")
        return PT::Float;
    if (t == "int"   || t == "int32_t" || t == "uint32_t" || t == "int64_t" ||
        t == "uint64_t" || t == "uint8_t" || t == "int16_t" || t == "uint16_t" ||
        t == "size_t" || t == "ptrdiff_t")
        return PT::Int;
    if (t == "bool")
        return PT::Bool;
    if (t == "Vector3" || t == "Vec3" || t == "glm_vec3")
        return PT::Vec3;
    return PT::Any;
}

// --- Graph builder -----------------------------------------------------------

struct BuildState
{
    float X = 200.0f;
    float Y = 400.0f;   // exec chain Y
    float DataY = 200.0f; // data nodes Y (above exec chain)
    int PrevExecNodeID = -1;
    int PrevExecPinID  = -1;

    float NextX() { float r = X; X += 220.0f; return r; }
};

static void EmitStmts(NodeGraphCanvas& g, const std::vector<Stmt>& stmts, BuildState& bs);

static void EmitStmt(NodeGraphCanvas& g, const Stmt& s, BuildState& bs)
{
    using NK = NodeGraphCanvas::NodeKind;

    auto linkExec = [&](int nodeID, int inPinID)
    {
        if (bs.PrevExecNodeID != -1 && bs.PrevExecPinID != -1)
            g.AddExecLink(bs.PrevExecNodeID, bs.PrevExecPinID, nodeID, inPinID);
    };

    switch (s.Kind)
    {
        case StmtKind::Decl:
        {
            // s.Raw = C++ type name, s.Lhs = variable name, s.Rhs = init expression
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
            n.Pins.push_back({ inPin,  NodeGraphCanvas::PinType::Exec, NodeGraphCanvas::PinDir::Input,  {} });
            snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "In");
            n.Pins.push_back({ outPin, NodeGraphCanvas::PinType::Exec, NodeGraphCanvas::PinDir::Output, {} });
            snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "Out");

            NodeGraphCanvas::PinType valType = MapCppTypeToPinType(s.Raw);
            n.Pins.push_back({ valPin, valType, NodeGraphCanvas::PinDir::Output, {} });
            snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "Value");
            if (valType == NodeGraphCanvas::PinType::Any)
                n.Pins.back().TypeName = s.Raw; // store full C++ type for display

            int nodeID = n.ID;
            g.AddNode(std::move(n));
            linkExec(nodeID, inPin);

            bs.PrevExecNodeID = nodeID;
            bs.PrevExecPinID  = outPin;
            break;
        }

        case StmtKind::Return:
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

        case StmtKind::If:
        {
            // Emit Branch node
            NodeGraphCanvas::ScriptNode branch;
            branch.ID   = g.NewID();
            branch.Kind = NK::Branch;
            branch.PosX = bs.NextX();
            branch.PosY = bs.Y;
            snprintf(branch.Title, sizeof(branch.Title), "Branch");

            int inPin   = g.NewID();
            int condPin = g.NewID();
            int truePin = g.NewID();
            int falsePin = g.NewID();
            branch.Pins.push_back({ inPin,    NodeGraphCanvas::PinType::Exec,  NodeGraphCanvas::PinDir::Input,  {} });
            snprintf(branch.Pins.back().Name, sizeof(branch.Pins.back().Name), "In");
            branch.Pins.push_back({ condPin,  NodeGraphCanvas::PinType::Bool,  NodeGraphCanvas::PinDir::Input,  {} });
            snprintf(branch.Pins.back().Name, sizeof(branch.Pins.back().Name), "Condition");
            branch.Pins.push_back({ truePin,  NodeGraphCanvas::PinType::Exec,  NodeGraphCanvas::PinDir::Output, {} });
            snprintf(branch.Pins.back().Name, sizeof(branch.Pins.back().Name), "True");
            branch.Pins.push_back({ falsePin, NodeGraphCanvas::PinType::Exec,  NodeGraphCanvas::PinDir::Output, {} });
            snprintf(branch.Pins.back().Name, sizeof(branch.Pins.back().Name), "False");

            int branchID = branch.ID;
            float branchPosX = branch.PosX;
            g.AddNode(std::move(branch));
            linkExec(branchID, inPin);

            // Emit condition expression if simple enough
            if (!s.Cond.empty())
            {
                ExprResult cond = BuildExprNode(g, s.Cond, branchPosX - 160.0f, bs.DataY - 60.0f);
                if (cond.NodeID != -1)
                    g.AddDataLink(cond.NodeID, cond.PinID, branchID, condPin);
            }

            // True branch — emit below at offset Y
            BuildState trueBs;
            trueBs.X              = bs.X;
            trueBs.Y              = bs.Y + 140.0f;
            trueBs.DataY          = bs.Y + 60.0f;
            trueBs.PrevExecNodeID = branchID;
            trueBs.PrevExecPinID  = truePin;
            EmitStmts(g, s.Then, trueBs);

            // False branch (else) at further offset
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

            // After branch, advance X past sub-graphs; no single exec continuation
            bs.X = std::max(trueBs.X, bs.X) + 20.0f;
            bs.PrevExecNodeID = -1;
            bs.PrevExecPinID  = -1;
            break;
        }

        case StmtKind::Assign:
        {
            // Emit SetProperty node
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
            n.Pins.push_back({ inPin,  NodeGraphCanvas::PinType::Exec,  NodeGraphCanvas::PinDir::Input,  {} });
            snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "In");
            n.Pins.push_back({ valPin, NodeGraphCanvas::PinType::Float, NodeGraphCanvas::PinDir::Input,  {} });
            snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "Value");
            n.Pins.push_back({ outPin, NodeGraphCanvas::PinType::Exec,  NodeGraphCanvas::PinDir::Output, {} });
            snprintf(n.Pins.back().Name, sizeof(n.Pins.back().Name), "Out");

            int nodeID = n.ID;
            float nodePosX = n.PosX;
            g.AddNode(std::move(n));
            linkExec(nodeID, inPin);

            // Try to emit an expression node for the RHS
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

        default: // Call / Raw — emit as RawCode node
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
            n.Pins.push_back({ inPin,  NodeGraphCanvas::PinType::Exec, NodeGraphCanvas::PinDir::Input,  {} });
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

static void EmitStmts(NodeGraphCanvas& g, const std::vector<Stmt>& stmts, BuildState& bs)
{
    for (const auto& s : stmts)
        EmitStmt(g, s, bs);
}

// --- Entry point -------------------------------------------------------------

static void ParseMethodIntoGraph(const std::string& src, const char* signature,
                                  NodeGraphCanvas& g, NodeGraphCanvas::NodeKind eventKind)
{
    size_t sigPos = src.find(signature);
    if (sigPos == std::string::npos) return;

    // Find the opening brace of the method body
    size_t bracePos = src.find('{', sigPos);
    if (bracePos == std::string::npos) return;

    // Extract balanced braces content
    int depth = 0;
    size_t bodyStart = bracePos;
    size_t bodyEnd   = bracePos;
    for (size_t i = bracePos; i < src.size(); ++i)
    {
        if (src[i] == '{') ++depth;
        if (src[i] == '}') { --depth; if (depth == 0) { bodyEnd = i; break; } }
    }

    std::string body = src.substr(bodyStart, bodyEnd - bodyStart + 1);

    auto toks = Tokenize(body);
    TStream ts{ toks, 0 };
    auto stmts = ParseBlock(ts);

    // The event node is already in the graph — find it and hook up the exec chain
    int eventNodeID = -1;
    int eventExecPinID = -1;
    for (const auto& n : g.Nodes)
    {
        if (n.Kind == eventKind)
        {
            eventNodeID = n.ID;
            for (const auto& p : n.Pins)
                if (p.Dir == NodeGraphCanvas::PinDir::Output && p.Type == NodeGraphCanvas::PinType::Exec)
                    eventExecPinID = p.ID;
            break;
        }
    }

    BuildState bs;
    bs.X              = 340.0f;
    bs.Y              = 120.0f;
    bs.DataY          = 40.0f;
    bs.PrevExecNodeID = eventNodeID;
    bs.PrevExecPinID  = eventExecPinID;

    EmitStmts(g, stmts, bs);
}

} // namespace CppGraph_REMOVED_SENTINEL_DO_NOT_USE
#endif

// ---------------------------------------------------------------------------
// File parser — populates Views, Owned, and graphs from an existing header
// ---------------------------------------------------------------------------

static void ParseConstructFile(const char* filePath, ConstructDoc& doc)
{
	std::ifstream f(filePath, std::ios::binary);
	if (!f.is_open()) return;
	std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

	auto extractWord = [&](size_t pos) -> std::pair<std::string, size_t>
	{
		while (pos < src.size() && std::isspace((unsigned char)src[pos]))
			++pos;
		size_t start = pos;
		while (pos < src.size() && (std::isalnum((unsigned char)src[pos]) || src[pos] == '_'))
			++pos;
		return { src.substr(start, pos - start), pos };
	};

	// ConstructView<T> Name
	{
		const std::string key = "ConstructView<";
		size_t pos            = 0;
		while ((pos = src.find(key, pos)) != std::string::npos)
		{
			pos += key.size();
			size_t gtPos = src.find('>', pos);
			if (gtPos == std::string::npos) break;
			std::string typeName    = src.substr(pos, gtPos - pos);
			auto [memberName, next] = extractWord(gtPos + 1);
			if (!typeName.empty() && !memberName.empty())
			{
				ConstructDoc::ViewEntry v = {};
				strncpy(v.TypeName, typeName.c_str(), sizeof(v.TypeName) - 1);
				strncpy(v.MemberName, memberName.c_str(), sizeof(v.MemberName) - 1);
				doc.Views.push_back(v);
			}
			pos = gtPos + 1;
		}
	}

	// Owned<T> Name
	{
		const std::string key = "Owned<";
		size_t pos            = 0;
		while ((pos = src.find(key, pos)) != std::string::npos)
		{
			pos += key.size();
			size_t gtPos = src.find('>', pos);
			if (gtPos == std::string::npos) break;
			std::string typeName    = src.substr(pos, gtPos - pos);
			auto [memberName, next] = extractWord(gtPos + 1);
			if (!typeName.empty() && !memberName.empty())
			{
				ConstructDoc::OwnedEntry o = {};
				strncpy(o.TypeName, typeName.c_str(), sizeof(o.TypeName) - 1);
				strncpy(o.MemberName, memberName.c_str(), sizeof(o.MemberName) - 1);
				doc.Owned.push_back(o);
			}
			pos = gtPos + 1;
		}
	}

	// Scan for lifecycle hooks and TNXFUNC methods via TrinyxParser DLL
	auto funcs = TrinyxParser::ScanFile(src);

	for (int i = 0; i < static_cast<int>(ConstructDoc::TickSlot::Count); ++i)
		doc.Graphs[i].Clear();
	doc.UserFuncs.clear();

	for (const auto& func : funcs)
	{
		if (func.IsLifecycle)
		{
			int slot = LifecycleToSlotIndex(func.Lifecycle);
			if (slot < 0) continue;
			NodeGraphCanvas::NodeKind ek = LifecycleToEventKind(func.Lifecycle);
			doc.Graphs[slot].AddEventNode(ek, 80.0f, 120.0f);

			int entryNodeID = -1, entryExecPinID = -1;
			for (const auto& n : doc.Graphs[slot].Nodes)
			{
				if (n.Kind == ek)
				{
					entryNodeID = n.ID;
					for (const auto& p : n.Pins)
						if (p.Dir == NodeGraphCanvas::PinDir::Output && p.Type == NodeGraphCanvas::PinType::Exec)
							entryExecPinID = p.ID;
					break;
				}
			}

			TrinyxParser::ParsedMethod pm = TrinyxParser::ParseMethodBody(src, func.Signature);
			BuildGraphFromParsedMethod(pm, doc.Graphs[slot], entryNodeID, entryExecPinID);
		}
		else
		{
			ConstructDoc::UserFuncTab tab;
			tab.Name        = func.Name;
			tab.DisplayName = func.DisplayName;
			tab.Signature   = func.Signature;
			tab.Graph.Clear();
			tab.Graph.AddFuncEntryNode(func.DisplayName.c_str(), 80.0f, 120.0f);

			int entryNodeID = -1, entryExecPinID = -1;
			for (const auto& n : tab.Graph.Nodes)
			{
				if (n.Kind == NodeGraphCanvas::NodeKind::FuncEntry)
				{
					entryNodeID = n.ID;
					for (const auto& p : n.Pins)
						if (p.Dir == NodeGraphCanvas::PinDir::Output && p.Type == NodeGraphCanvas::PinType::Exec)
							entryExecPinID = p.ID;
					break;
				}
			}

			TrinyxParser::ParsedMethod pm = TrinyxParser::ParseMethodBody(src, func.Signature);
			BuildGraphFromParsedMethod(pm, tab.Graph, entryNodeID, entryExecPinID);
			doc.UserFuncs.push_back(std::move(tab));
		}
	}
}

// ---------------------------------------------------------------------------
// ConstructDoc
// ---------------------------------------------------------------------------

ConstructDoc::ConstructDoc(const char* typeName, const char* filePath)
	: TypeName(typeName)
{
	for (int i = 0; i < static_cast<int>(TickSlot::Count); ++i)
		Graphs[i].Clear();

	if (filePath && filePath[0])
	{
		FilePath = filePath;
		strncpy(OutputPath, filePath, sizeof(OutputPath) - 1);
		ParseConstructFile(filePath, *this);
	}
	else
	{
		// New construct — seed all slots with default event nodes.
		using NK                     = NodeGraphCanvas::NodeKind;
		static const NK SlotEvents[] = {
			NK::Event_OnPrePhysics,
			NK::Event_OnPhysicsStep,
			NK::Event_OnPostPhysics,
			NK::Event_OnUpdate,
		};
		for (int i = 0; i < static_cast<int>(TickSlot::Count); ++i)
			Graphs[i].AddEventNode(SlotEvents[i], 80.0f, 120.0f);
	}
}

// ---------------------------------------------------------------------------
// ConstructEditorWindow
// ---------------------------------------------------------------------------

ConstructEditorWindow::ConstructEditorWindow()
	: EditorPanel("Construct Editor")
{
}

void ConstructEditorWindow::OpenConstruct(const char* typeName, const char* filePath)
{
	for (int i = 0; i < static_cast<int>(Docs.size()); ++i)
	{
		if (Docs[i].TypeName == typeName)
		{
			ActiveTab = i;
			bVisible  = true;
			return;
		}
	}
	Docs.emplace_back(typeName, filePath);
	ActiveTab = static_cast<int>(Docs.size()) - 1;
	bVisible  = true;
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

void ConstructEditorWindow::Draw(EditorState& state)
{
	if (!BeginPadded(ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		ImGui::End();
		return;
	}

	TnxWidgets::PanelHeader(nullptr, "Construct Editor");

	// --- Top toolbar ---
	if (ImGui::Button("+ New"))
		bShowNewDialog = true;

	if (bShowNewDialog)
	{
		ImGui::SameLine();
		ImGui::SetNextItemWidth(160.0f);
		ImGui::InputText("##NewName", NewTypeBuf, sizeof(NewTypeBuf));
		ImGui::SameLine();
		if (ImGui::Button("Create"))
		{
			if (NewTypeBuf[0] != '\0')
			{
				OpenConstruct(NewTypeBuf);
				bShowNewDialog = false;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) bShowNewDialog = false;
	}

	if (Docs.empty())
	{
		ImGui::Separator();
		ImGui::TextDisabled("No Constructs open. Click '+ New' or open one from the Content Browser.");
		ImGui::End();
		return;
	}

	// --- Tab bar + pane toggles on the same line ---
	DrawTabBar();

	if (ActiveTab < 0 || ActiveTab >= static_cast<int>(Docs.size()))
	{
		ImGui::End();
		return;
	}

	ConstructDoc& doc = Docs[static_cast<size_t>(ActiveTab)];

	// Pane toggle buttons — drawn on their own line, right-aligned before the separator.
	{
		float spacing   = ImGui::GetStyle().ItemSpacing.x;
		float btnW      = ImGui::CalcTextSize("[ Comp ]").x + ImGui::GetStyle().FramePadding.x * 2.0f;
		float totalBtns = btnW * 2.0f + spacing;
		ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - totalBtns);
		if (ImGui::SmallButton(bLeftPaneVisible ? "[ Comp ]" : "[ > ]")) bLeftPaneVisible = !bLeftPaneVisible;
		ImGui::SameLine();
		if (ImGui::SmallButton(bRightPaneVisible ? "[ Info ]" : "[ < ]")) bRightPaneVisible = !bRightPaneVisible;
	}

	ImGui::Separator();

	// --- Three-column layout: Composition | Node Graph | Info ---
	float totalH = ImGui::GetContentRegionAvail().y;

	if (bLeftPaneVisible)
	{
		DrawCompositionPanel(doc, totalH);
		ImGui::SameLine();
	}
	DrawSlotTabs(doc, 0.0f, totalH);
	if (bRightPaneVisible)
	{
		ImGui::SameLine();
		DrawInfoPanel(doc, totalH, state);
	}

	ImGui::End();
}

void ConstructEditorWindow::DrawTabBar()
{
	if (ImGui::BeginTabBar("##ConstructTabs"))
	{
		for (int i = 0; i < static_cast<int>(Docs.size()); ++i)
		{
			bool open               = true;
			ImGuiTabItemFlags flags = (i == ActiveTab) ? ImGuiTabItemFlags_SetSelected : 0;
			if (ImGui::BeginTabItem(Docs[static_cast<size_t>(i)].TypeName.c_str(), &open, flags))
			{
				ActiveTab = i;
				ImGui::EndTabItem();
			}
			if (!open)
			{
				Docs.erase(Docs.begin() + i);
				if (ActiveTab >= static_cast<int>(Docs.size())) ActiveTab = static_cast<int>(Docs.size()) - 1;
				break;
			}
		}
		ImGui::EndTabBar();
	}
}

void ConstructEditorWindow::DrawCompositionPanel(ConstructDoc& doc, float height)
{
	ImGui::BeginChild("##ConstructComp", ImVec2(180.0f, height), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);

	ImGui::TextDisabled("Views");
	ImGui::SameLine();
	if (ImGui::SmallButton("+##AddView"))
		doc.Views.push_back({});
	ImGui::Separator();

	for (int i = 0; i < static_cast<int>(doc.Views.size()); ++i)
	{
		auto& v = doc.Views[static_cast<size_t>(i)];
		ImGui::PushID(i);
		ImGui::SetNextItemWidth(80.0f);
		ImGui::InputText("##VType", v.TypeName, sizeof(v.TypeName));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(60.0f);
		ImGui::InputText("##VName", v.MemberName, sizeof(v.MemberName));
		ImGui::SameLine();
		if (ImGui::SmallButton("x"))
		{
			doc.Views.erase(doc.Views.begin() + i);
			ImGui::PopID();
			break;
		}
		ImGui::PopID();
	}

	ImGui::Spacing();
	ImGui::TextDisabled("Owned<T>");
	ImGui::SameLine();
	if (ImGui::SmallButton("+##AddOwned"))
		doc.Owned.push_back({});
	ImGui::Separator();

	for (int i = 0; i < static_cast<int>(doc.Owned.size()); ++i)
	{
		auto& o = doc.Owned[static_cast<size_t>(i)];
		ImGui::PushID(1000 + i);
		ImGui::SetNextItemWidth(80.0f);
		ImGui::InputText("##OType", o.TypeName, sizeof(o.TypeName));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(60.0f);
		ImGui::InputText("##OName", o.MemberName, sizeof(o.MemberName));
		ImGui::SameLine();
		if (ImGui::SmallButton("x"))
		{
			doc.Owned.erase(doc.Owned.begin() + i);
			ImGui::PopID();
			break;
		}
		ImGui::PopID();
	}

	ImGui::EndChild();
}

void ConstructEditorWindow::DrawSlotTabs(ConstructDoc& doc, float /*width*/, float height)
{
	ImGui::BeginChild("##ConstructCenter", ImVec2(0.0f, height), ImGuiChildFlags_ResizeX,
		ImGuiWindowFlags_NoScrollbar);

	if (ImGui::BeginTabBar("##SlotTabs"))
	{
		for (int s = 0; s < static_cast<int>(ConstructDoc::TickSlot::Count); ++s)
		{
			if (ImGui::BeginTabItem(SlotNames[s]))
			{
				doc.ActiveSlot     = s;
				doc.ActiveUserFunc = -1;
				ImGui::EndTabItem();
			}
		}
		for (int u = 0; u < static_cast<int>(doc.UserFuncs.size()); ++u)
		{
			const char* label = doc.UserFuncs[static_cast<size_t>(u)].DisplayName.c_str();
			if (ImGui::BeginTabItem(label))
			{
				doc.ActiveSlot     = -1;
				doc.ActiveUserFunc = u;
				ImGui::EndTabItem();
			}
		}
		ImGui::EndTabBar();
	}

	float graphH = ImGui::GetContentRegionAvail().y;
	float graphW = ImGui::GetContentRegionAvail().x;

	// Select the active graph: lifecycle slot or user func
	NodeGraphCanvas* activeGraph = nullptr;
	char canvasID[64];
	if (doc.ActiveUserFunc >= 0 && doc.ActiveUserFunc < static_cast<int>(doc.UserFuncs.size()))
	{
		activeGraph = &doc.UserFuncs[static_cast<size_t>(doc.ActiveUserFunc)].Graph;
		snprintf(canvasID, sizeof(canvasID), "ce_%p_u%d", (void*)&doc, doc.ActiveUserFunc);
	}
	else if (doc.ActiveSlot >= 0 && doc.ActiveSlot < static_cast<int>(ConstructDoc::TickSlot::Count))
	{
		activeGraph = &doc.Graphs[doc.ActiveSlot];
		snprintf(canvasID, sizeof(canvasID), "ce_%p_%d", (void*)&doc, doc.ActiveSlot);
	}

	if (activeGraph)
		activeGraph->DrawCanvas(canvasID, graphW, graphH);

	ImGui::EndChild();
}

void ConstructEditorWindow::DrawInfoPanel(ConstructDoc& doc, float height, EditorState& state)
{
	ImGui::BeginChild("##ConstructInfo", ImVec2(0.0f, height), ImGuiChildFlags_Borders);

	ImGui::TextDisabled("Type");
	ImGui::TextUnformatted(doc.TypeName.c_str());
	ImGui::Spacing();

	ImGui::TextDisabled("Output Path");
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputText("##OutPathCE", doc.OutputPath, sizeof(doc.OutputPath));

	ImGui::Spacing();
	if (ImGui::Button("Export .h##CEExp", ImVec2(-1.0f, 0.0f)))
	{
		// TODO: code generation from node graphs
		snprintf(doc.StatusMsg, sizeof(doc.StatusMsg), "Code gen not yet implemented.");
		doc.bStatusError = true;
	}

	if (doc.StatusMsg[0] != '\0')
	{
		ImVec4 col = doc.bStatusError ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f) : ImVec4(0.35f, 1.0f, 0.35f, 1.0f);
		ImGui::TextColored(col, "%s", doc.StatusMsg);
	}

	ImGui::Spacing();
	ImGui::TextDisabled("Tick slots");
	for (int s = 0; s < static_cast<int>(ConstructDoc::TickSlot::Count); ++s)
	{
		int nodeCount = static_cast<int>(doc.Graphs[s].Nodes.size());
		ImGui::Text("  %s: %d node%s", SlotNames[s], nodeCount, nodeCount == 1 ? "" : "s");
	}

	if (!doc.UserFuncs.empty())
	{
		ImGui::Spacing();
		ImGui::TextDisabled("TNXFUNC methods");
		for (const auto& f : doc.UserFuncs)
		{
			int nodeCount = static_cast<int>(f.Graph.Nodes.size());
			ImGui::Text("  %s: %d node%s", f.DisplayName.c_str(), nodeCount, nodeCount == 1 ? "" : "s");
		}
	}

	ImGui::EndChild();
}
