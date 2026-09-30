#include "TrinyxParser.h"

#include <cctype>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

enum class TK : uint8_t
{
	Ident,
	Number,
	Str,
	LParen,
	RParen,
	LBrace,
	RBrace,
	Semi,
	Comma,
	Dot,
	Assign,
	CompEq,
	CompNe,
	CompLt,
	CompLe,
	CompGt,
	CompGe,
	AmpAmp,
	PipePipe,
	Bang,
	Plus,
	Minus,
	Star,
	Slash,
	Percent,
	Arrow,
	Colon,
	ColonColon,
	Eof,
	Other
};

struct Token
{
	TK Kind;
	std::string Text;
};

static std::vector<Token> Tokenize(const std::string& src)
{
	std::vector<Token> toks;
	size_t i = 0, n = src.size();
	while (i < n)
	{
		if (std::isspace((unsigned char)src[i]))
		{
			++i;
			continue;
		}
		if (i + 1 < n && src[i] == '/' && src[i + 1] == '/')
		{
			while (i < n && src[i] != '\n')
				++i;
			continue;
		}
		if (i + 1 < n && src[i] == '/' && src[i + 1] == '*')
		{
			i += 2;
			while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/'))
				++i;
			if (i + 1 < n) i += 2;
			continue;
		}
		if (std::isalpha((unsigned char)src[i]) || src[i] == '_')
		{
			size_t s = i;
			while (i < n && (std::isalnum((unsigned char)src[i]) || src[i] == '_'))
				++i;
			toks.push_back({ TK::Ident, src.substr(s, i - s) });
			continue;
		}
		if (std::isdigit((unsigned char)src[i]) || (src[i] == '.' && i + 1 < n && std::isdigit((unsigned char)src[i + 1])))
		{
			size_t s = i;
			while (i < n && (std::isdigit((unsigned char)src[i]) || src[i] == '.' || src[i] == 'f' || src[i] == 'u' || src[i] == 'L'))
				++i;
			toks.push_back({ TK::Number, src.substr(s, i - s) });
			continue;
		}
		if (src[i] == '"')
		{
			size_t s = i++;
			while (i < n && src[i] != '"')
			{
				if (src[i] == '\\') ++i;
				++i;
			}
			if (i < n) ++i;
			toks.push_back({ TK::Str, src.substr(s, i - s) });
			continue;
		}
		if (i + 1 < n)
		{
			char a = src[i], b = src[i + 1];
			if (a == '=' && b == '=')
			{
				toks.push_back({ TK::CompEq, "==" });
				i += 2;
				continue;
			}
			if (a == '!' && b == '=')
			{
				toks.push_back({ TK::CompNe, "!=" });
				i += 2;
				continue;
			}
			if (a == '<' && b == '=')
			{
				toks.push_back({ TK::CompLe, "<=" });
				i += 2;
				continue;
			}
			if (a == '>' && b == '=')
			{
				toks.push_back({ TK::CompGe, ">=" });
				i += 2;
				continue;
			}
			if (a == '&' && b == '&')
			{
				toks.push_back({ TK::AmpAmp, "&&" });
				i += 2;
				continue;
			}
			if (a == '|' && b == '|')
			{
				toks.push_back({ TK::PipePipe, "||" });
				i += 2;
				continue;
			}
			if (a == '-' && b == '>')
			{
				toks.push_back({ TK::Arrow, "->" });
				i += 2;
				continue;
			}
			if (a == ':' && b == ':')
			{
				toks.push_back({ TK::ColonColon, "::" });
				i += 2;
				continue;
			}
		}
		TK kind = TK::Other;
		std::string text(1, src[i]);
		switch (src[i])
		{
			case '(': kind = TK::LParen; break;
			case ')': kind = TK::RParen; break;
			case '{': kind = TK::LBrace; break;
			case '}': kind = TK::RBrace; break;
			case ';': kind = TK::Semi; break;
			case ',': kind = TK::Comma; break;
			case '.': kind = TK::Dot; break;
			case '=': kind = TK::Assign; break;
			case '<': kind = TK::CompLt; break;
			case '>': kind = TK::CompGt; break;
			case '!': kind = TK::Bang; break;
			case '+': kind = TK::Plus; break;
			case '-': kind = TK::Minus; break;
			case '*': kind = TK::Star; break;
			case '/': kind = TK::Slash; break;
			case '%': kind = TK::Percent; break;
			case ':': kind = TK::Colon; break;
			default: break;
		}
		toks.push_back({ kind, text });
		++i;
	}
	toks.push_back({ TK::Eof, "" });
	return toks;
}

// ---------------------------------------------------------------------------
// Token stream
// ---------------------------------------------------------------------------

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

	std::string CollectUntil(std::initializer_list<TK> stopKinds, int depth = 0)
	{
		std::string out;
		while (!At(TK::Eof))
		{
			TK k = Peek().Kind;
			if (k == TK::LParen || k == TK::LBrace) ++depth;
			if (k == TK::RParen || k == TK::RBrace)
			{
				if (depth <= 0) break;
				--depth;
			}
			for (TK sk : stopKinds)
				if (k == sk && depth == 0)
				{
					if (!out.empty() && out.back() == ' ') out.pop_back();
					return out;
				}
			if (!out.empty()) out += ' ';
			out += Consume().Text;
		}
		return out;
	}

	std::string ConsumeBlock()
	{
		if (!At(TK::LBrace)) return {};
		Consume();
		std::string out;
		int depth = 0;
		while (!At(TK::Eof))
		{
			if (At(TK::LBrace))
			{
				++depth;
				out += "{ ";
				Consume();
				continue;
			}
			if (At(TK::RBrace))
			{
				if (depth == 0)
				{
					Consume();
					break;
				}
				--depth;
				out += "} ";
				Consume();
				continue;
			}
			out += Peek().Text + " ";
			Consume();
		}
		return out;
	}
};

// ---------------------------------------------------------------------------
// Statement parser
// ---------------------------------------------------------------------------

static std::vector<TrinyxParser::Stmt> ParseBlock(TStream& ts);

static TrinyxParser::Stmt ParseStmt(TStream& ts)
{
	using S  = TrinyxParser::Stmt;
	using SK = TrinyxParser::StmtKind;
	S s;

	if (ts.AtIdent("return"))
	{
		ts.Consume();
		s.Kind = SK::Return;
		s.Raw  = ts.CollectUntil({ TK::Semi });
		if (ts.At(TK::Semi)) ts.Consume();
		return s;
	}

	if (ts.AtIdent("if"))
	{
		ts.Consume();
		if (ts.At(TK::LParen)) ts.Consume();
		s.Cond = ts.CollectUntil({ TK::RParen }, 1);
		if (ts.At(TK::RParen)) ts.Consume();
		s.Kind = SK::If;
		if (ts.At(TK::LBrace))
			s.Then = ParseBlock(ts);
		else
			s.Then.push_back(ParseStmt(ts));
		if (ts.AtIdent("else"))
		{
			ts.Consume();
			if (ts.At(TK::LBrace))
				s.Else = ParseBlock(ts);
			else
				s.Else.push_back(ParseStmt(ts));
		}
		return s;
	}

	if (ts.AtIdent("for") || ts.AtIdent("while") || ts.AtIdent("do"))
	{
		s.Kind = SK::Raw;
		s.Raw  = ts.Consume().Text;
		if (ts.At(TK::LParen))
		{
			ts.Consume();
			s.Raw += "(" + ts.CollectUntil({ TK::RParen }, 1) + ")";
			if (ts.At(TK::RParen)) ts.Consume();
		}
		if (ts.At(TK::LBrace)) s.Raw += " { ... }";
		ts.ConsumeBlock();
		return s;
	}

	// Decl detection: optional const, type ident, optional template args, var ident
	{
		int saved   = ts.Pos;
		bool isDecl = false;
		if (ts.AtIdent("const")) ts.Consume();
		if (ts.At(TK::Ident))
		{
			ts.Consume();
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
				if (ts.At(TK::Other) && ts.Peek().Text == "[")
				{
					ts.Consume();
					ts.CollectUntil({ TK::Other });
					ts.Consume();
				}
				if (ts.At(TK::Assign) || ts.At(TK::Semi) || ts.At(TK::LBrace))
					isDecl = true;
			}
		}
		ts.Pos = saved;
		if (isDecl)
		{
			s.Kind = SK::Decl;
			if (ts.AtIdent("const")) ts.Consume();
			if (ts.At(TK::Ident)) s.Raw = ts.Consume().Text; // base type name
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
			if (ts.At(TK::Ident)) s.Lhs = ts.Consume().Text;
			if (ts.At(TK::Other) && ts.Peek().Text == "[")
			{
				ts.Consume();
				ts.CollectUntil({ TK::Other });
				ts.Consume();
			}
			if (ts.At(TK::Assign))
			{
				ts.Consume();
				s.Rhs = ts.CollectUntil({ TK::Semi });
			}
			if (ts.At(TK::Semi)) ts.Consume();
			return s;
		}
	}

	// Assignment: memberPath = expr;
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
				ts.Consume();
				s.Kind = SK::Assign;
				s.Lhs  = lhs;
				s.Rhs  = ts.CollectUntil({ TK::Semi });
				if (ts.At(TK::Semi)) ts.Consume();
				return s;
			}
		}
		ts.Pos = saved;
	}

	s.Kind = SK::Call;
	s.Raw  = ts.CollectUntil({ TK::Semi, TK::RBrace });
	if (ts.At(TK::Semi)) ts.Consume();
	return s;
}

static std::vector<TrinyxParser::Stmt> ParseBlock(TStream& ts)
{
	std::vector<TrinyxParser::Stmt> stmts;
	if (ts.At(TK::LBrace)) ts.Consume();
	while (!ts.At(TK::Eof) && !ts.At(TK::RBrace))
		stmts.push_back(ParseStmt(ts));
	if (ts.At(TK::RBrace)) ts.Consume();
	return stmts;
}

// ---------------------------------------------------------------------------
// TrinyxParser::ScanFile
// ---------------------------------------------------------------------------

std::vector<TrinyxParser::FuncDescriptor> TrinyxParser::ScanFile(const std::string& src)
{
	std::vector<FuncDescriptor> result;
	using LE = LifecycleEvent;

	static const struct
	{
		const char* sig;
		LE lc;
		const char* display;
		const char* name;
	} Lifecycle[] = {
		{ "void PrePhysics(", LE::PrePhysics, "Pre-Physics", "PrePhysics" },
		{ "void PhysicsStep(", LE::PhysicsStep, "Physics Step", "PhysicsStep" },
		{ "void PostPhysics(", LE::PostPhysics, "Post-Physics", "PostPhysics" },
		{ "void ScalarUpdate(", LE::ScalarUpdate, "Scalar Update", "ScalarUpdate" },
		{ "void OnSpawn(", LE::OnSpawn, "On Spawn", "OnSpawn" },
		{ "void OnDestroy(", LE::OnDestroy, "On Destroy", "OnDestroy" },
	};

	for (const auto& lc : Lifecycle)
	{
		if (src.find(lc.sig) == std::string::npos) continue;
		FuncDescriptor d;
		d.Name        = lc.name;
		d.DisplayName = lc.display;
		d.Signature   = lc.sig;
		d.IsLifecycle = true;
		d.Lifecycle   = lc.lc;
		result.push_back(std::move(d));
	}

	// TNXFUNC-tagged user methods
	size_t pos = 0;
	while ((pos = src.find("TNXFUNC", pos)) != std::string::npos)
	{
		// Must be followed by '(' — reject TNXFUNC_SOMETHING
		size_t after = pos + 7;
		while (after < src.size() && src[after] == ' ')
			++after;
		if (after >= src.size() || src[after] != '(')
		{
			++pos;
			continue;
		}

		// Consume TNXFUNC(...)
		size_t argStart = after + 1;
		int depth       = 1;
		size_t argEnd   = argStart;
		while (argEnd < src.size() && depth > 0)
		{
			if (src[argEnd] == '(')
				++depth;
			else if (src[argEnd] == ')')
				--depth;
			++argEnd;
		}

		// Extract optional DisplayName = "..."
		std::string displayName;
		{
			std::string args = src.substr(argStart, argEnd - argStart - 1);
			size_t dnPos     = args.find("DisplayName");
			if (dnPos != std::string::npos)
			{
				size_t eqPos = args.find('=', dnPos + 11);
				size_t q1    = args.find('"', eqPos + 1);
				size_t q2    = (q1 != std::string::npos) ? args.find('"', q1 + 1) : std::string::npos;
				if (q1 != std::string::npos && q2 != std::string::npos)
					displayName = args.substr(q1 + 1, q2 - q1 - 1);
			}
		}

		// Skip whitespace after TNXFUNC(...) then read up to '{' or ';'
		size_t sigStart = argEnd;
		while (sigStart < src.size() && std::isspace((unsigned char)src[sigStart]))
			++sigStart;
		size_t sigEnd = sigStart;
		while (sigEnd < src.size() && src[sigEnd] != '{' && src[sigEnd] != ';')
			++sigEnd;

		// Extract function name: identifier immediately before '('
		std::string sigText = src.substr(sigStart, sigEnd - sigStart);
		size_t parenPos     = sigText.find('(');
		if (parenPos == std::string::npos)
		{
			pos = argEnd;
			continue;
		}

		size_t nameEnd = parenPos;
		while (nameEnd > 0 && std::isspace((unsigned char)sigText[nameEnd - 1]))
			--nameEnd;
		size_t nameStart = nameEnd;
		while (nameStart > 0 && (std::isalnum((unsigned char)sigText[nameStart - 1]) || sigText[nameStart - 1] == '_'))
			--nameStart;

		std::string funcName = sigText.substr(nameStart, nameEnd - nameStart);
		if (funcName.empty())
		{
			pos = argEnd;
			continue;
		}

		// Skip lifecycle names — TNXFUNC on a lifecycle method is redundant
		bool isLifecycleName = false;
		for (const auto& lc : Lifecycle)
			if (funcName == lc.name)
			{
				isLifecycleName = true;
				break;
			}
		if (isLifecycleName)
		{
			pos = argEnd;
			continue;
		}

		if (displayName.empty()) displayName = funcName;

		FuncDescriptor d;
		d.Name        = funcName;
		d.DisplayName = displayName;
		d.Signature   = funcName + "(";
		d.IsLifecycle = false;
		d.Lifecycle   = LifecycleEvent::None;
		result.push_back(std::move(d));

		pos = argEnd;
	}

	return result;
}

// ---------------------------------------------------------------------------
// TrinyxParser::ParseMethodBody
// ---------------------------------------------------------------------------

TrinyxParser::ParsedMethod TrinyxParser::ParseMethodBody(const std::string& src,
	const std::string& signature)
{
	ParsedMethod out;

	size_t sigPos = src.find(signature);
	if (sigPos == std::string::npos) return out;

	size_t bracePos = src.find('{', sigPos);
	if (bracePos == std::string::npos) return out;

	int depth      = 0;
	size_t bodyEnd = bracePos;
	for (size_t i = bracePos; i < src.size(); ++i)
	{
		if (src[i] == '{') ++depth;
		if (src[i] == '}')
		{
			--depth;
			if (depth == 0)
			{
				bodyEnd = i;
				break;
			}
		}
	}

	std::string body = src.substr(bracePos, bodyEnd - bracePos + 1);
	auto toks        = Tokenize(body);
	TStream ts{ toks, 0 };
	out.Body  = ParseBlock(ts);
	out.Found = true;
	return out;
}
