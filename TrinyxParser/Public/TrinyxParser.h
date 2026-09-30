#pragma once

// ---------------------------------------------------------------------------
// DLL export/import
// ---------------------------------------------------------------------------
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(TRINYX_PARSER_EXPORTS)
#define TNX_PARSER_API __declspec(dllexport)
#else
#define TNX_PARSER_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define TNX_PARSER_API __attribute__((visibility("default")))
#else
#define TNX_PARSER_API
#endif

#include <cstdint>
#include <string>
#include <vector>

/// TrinyxParser — source-file analysis engine.
///
/// This DLL has zero engine dependencies (no NodeGraphCanvas, no ImGui, no Jolt).
/// It is safe to link into both the editor (in-process) and a standalone precompiler.
///
/// Scope: scan Construct source files for TNXFUNC() annotations + lifecycle methods,
/// and parse method bodies into a lightweight statement IR.
/// Graph building (IR → NodeGraphCanvas) lives on the editor side.
namespace TrinyxParser
{

// ---------------------------------------------------------------------------
// Statement IR
// ---------------------------------------------------------------------------

enum class StmtKind : uint8_t
{
	If,
	Return,
	Assign,
	Call,
	Raw,
	Decl
};

/// Lightweight IR node for a single statement parsed from a C++ method body.
struct Stmt
{
	StmtKind Kind = StmtKind::Raw;
	std::string Cond;       ///< If: condition expression text
	std::string Lhs;        ///< Assign/Decl: left-hand side / variable name
	std::string Rhs;        ///< Assign/Decl: right-hand side / initializer
	std::string Raw;        ///< Call/Raw: verbatim text; Decl: C++ base type name
	std::vector<Stmt> Then; ///< If: true-branch body
	std::vector<Stmt> Else; ///< If: else-branch body
};

// ---------------------------------------------------------------------------
// Scan output
// ---------------------------------------------------------------------------

/// Built-in Construct lifecycle hooks, independent of NodeGraphCanvas::NodeKind.
/// The editor maps these to the appropriate event node kind.
enum class LifecycleEvent : uint8_t
{
	None = 0,
	PrePhysics,
	PhysicsStep,
	PostPhysics,
	ScalarUpdate,
	OnSpawn,
	OnDestroy,
};

/// Describes one function exposed to the node-graph editor.
struct FuncDescriptor
{
	std::string Name;        ///< C++ function name
	std::string DisplayName; ///< Tab label (TNXFUNC arg or Name)
	std::string Signature;   ///< Substring used to locate the method body
	bool IsLifecycle         = false;
	LifecycleEvent Lifecycle = LifecycleEvent::None;
};

/// Parsed method body returned by ParseMethodBody().
struct ParsedMethod
{
	std::vector<Stmt> Body;
	bool Found = false;
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/// Scan a Construct source file and return a descriptor for every exposed function:
/// - Built-in lifecycle methods present in the source
///   (PrePhysics, PhysicsStep, PostPhysics, ScalarUpdate)
/// - Methods annotated with TNXFUNC()
TNX_PARSER_API std::vector<FuncDescriptor> ScanFile(const std::string& src);

/// Parse a single method body into the TrinyxParser IR.
/// Searches src for the function by signature and extracts its statement tree.
TNX_PARSER_API ParsedMethod ParseMethodBody(const std::string& src, const std::string& signature);

} // namespace TrinyxParser
