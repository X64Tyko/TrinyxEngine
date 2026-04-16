#include "Json.h"
#include "Logger.h"
#include <cmath>
#include <cstdio>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════════════
// JsonValue — type queries and accessors
// ═══════════════════════════════════════════════════════════════════════════

JsonType JsonValue::Type() const
{
	switch (Data.index())
	{
		case 0: return JsonType::Null;
		case 1: return JsonType::Bool;
		case 2: return JsonType::Number;
		case 3: return JsonType::String;
		case 4: return JsonType::Array;
		case 5: return JsonType::Object;
		default: return JsonType::Null;
	}
}

bool JsonValue::IsNull() const { return std::holds_alternative<std::monostate>(Data); }
bool JsonValue::IsBool() const { return std::holds_alternative<bool>(Data); }
bool JsonValue::IsNumber() const { return std::holds_alternative<double>(Data); }
bool JsonValue::IsString() const { return std::holds_alternative<std::string>(Data); }
bool JsonValue::IsArray() const { return std::holds_alternative<ArrayType>(Data); }
bool JsonValue::IsObject() const { return std::holds_alternative<ObjectType>(Data); }

bool JsonValue::AsBool(bool Default) const
{
	auto* p = std::get_if<bool>(&Data);
	return p ? *p : Default;
}

double JsonValue::AsNumber(double Default) const
{
	auto* p = std::get_if<double>(&Data);
	return p ? *p : Default;
}

float JsonValue::AsFloat(float Default) const
{
	auto* p = std::get_if<double>(&Data);
	return p ? static_cast<float>(*p) : Default;
}

int JsonValue::AsInt(int Default) const
{
	auto* p = std::get_if<double>(&Data);
	return p ? static_cast<int>(*p) : Default;
}

const std::string& JsonValue::AsString() const
{
	static const std::string empty;
	auto* p = std::get_if<std::string>(&Data);
	return p ? *p : empty;
}

const JsonValue::ArrayType& JsonValue::AsArray() const
{
	static const ArrayType empty;
	auto* p = std::get_if<ArrayType>(&Data);
	return p ? *p : empty;
}

const JsonValue::ObjectType& JsonValue::AsObject() const
{
	static const ObjectType empty;
	auto* p = std::get_if<ObjectType>(&Data);
	return p ? *p : empty;
}

JsonValue::ArrayType& JsonValue::GetArray()
{
	if (!IsArray()) Data = ArrayType{};
	return std::get<ArrayType>(Data);
}

JsonValue::ObjectType& JsonValue::GetObject()
{
	if (!IsObject()) Data = ObjectType{};
	return std::get<ObjectType>(Data);
}

const JsonValue* JsonValue::Find(std::string_view Key) const
{
	auto* obj = std::get_if<ObjectType>(&Data);
	if (!obj) return nullptr;
	for (auto& [k, v] : *obj) if (k == Key) return &v;
	return nullptr;
}

JsonValue* JsonValue::Find(std::string_view Key)
{
	auto* obj = std::get_if<ObjectType>(&Data);
	if (!obj) return nullptr;
	for (auto& [k, v] : *obj) if (k == Key) return &v;
	return nullptr;
}

JsonValue& JsonValue::operator[](const std::string& Key)
{
	auto& obj = GetObject();
	for (auto& [k, v] : obj) if (k == Key) return v;
	obj.emplace_back(Key, Null());
	return obj.back().second;
}

// --- Factories ---

JsonValue JsonValue::Null() { return JsonValue{std::monostate{}}; }
JsonValue JsonValue::Bool(bool Val) { return JsonValue{Val}; }
JsonValue JsonValue::Number(double Val) { return JsonValue{Val}; }
JsonValue JsonValue::String(std::string Val) { return JsonValue{std::move(Val)}; }

JsonValue JsonValue::Array()
{
	JsonValue v;
	v.Data = ArrayType{};
	return v;
}

JsonValue JsonValue::Object()
{
	JsonValue v;
	v.Data = ObjectType{};
	return v;
}

// ═══════════════════════════════════════════════════════════════════════════
// Parser — recursive descent
// ═══════════════════════════════════════════════════════════════════════════

namespace
{
	struct ParseCtx
	{
		std::string_view Text;
		size_t Pos = 0;

		char Peek() const { return Pos < Text.size() ? Text[Pos] : '\0'; }
		char Next() { return Pos < Text.size() ? Text[Pos++] : '\0'; }
		bool AtEnd() const { return Pos >= Text.size(); }
	};

	void SkipWhitespace(ParseCtx& C)
	{
		while (C.Pos < C.Text.size())
		{
			char ch = C.Text[C.Pos];
			if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') ++C.Pos;
			else break;
		}
	}

	bool ParseError(ParseCtx& C, const char* Msg)
	{
		LOG_ENG_ERROR_F("[JSON] Parse error at position %zu: %s", C.Pos, Msg);
		return false;
	}

	// Forward declaration
	bool ParseValue(ParseCtx & C, JsonValue & Out);

	bool ParseString(ParseCtx& C, std::string& Out)
	{
		if (C.Next() != '"') return ParseError(C, "expected '\"'");

		Out.clear();
		while (!C.AtEnd())
		{
			char ch = C.Next();
			if (ch == '"') return true;
			if (ch == '\\')
			{
				char esc = C.Next();
				switch (esc)
				{
					case '"': Out += '"';
						break;
					case '\\': Out += '\\';
						break;
					case '/': Out += '/';
						break;
					case 'b': Out += '\b';
						break;
					case 'f': Out += '\f';
						break;
					case 'n': Out += '\n';
						break;
					case 'r': Out += '\r';
						break;
					case 't': Out += '\t';
						break;
					default: Out += '\\';
						Out += esc;
						break;
				}
			}
			else
			{
				Out += ch;
			}
		}
		return ParseError(C, "unterminated string");
	}

	bool ParseNumber(ParseCtx& C, double& Out)
	{
		size_t start = C.Pos;

		// Consume characters valid in a JSON number
		if (C.Peek() == '-') ++C.Pos;

		while (C.Pos < C.Text.size())
		{
			char ch = C.Text[C.Pos];
			if ((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E' || ch == '+' || ch == '-') ++C.Pos;
			else break;
		}

		if (C.Pos == start) return ParseError(C, "expected number");

		// Null-terminated copy for strtod (string_view is not null-terminated)
		char buf[64];
		size_t len = C.Pos - start;
		if (len >= sizeof(buf)) return ParseError(C, "number too long");
		std::memcpy(buf, C.Text.data() + start, len);
		buf[len] = '\0';

		char* end = nullptr;
		Out       = std::strtod(buf, &end);
		if (end != buf + len) return ParseError(C, "invalid number");

		return true;
	}

	bool ParseObject(ParseCtx& C, JsonValue& Out)
	{
		C.Next(); // consume '{'
		Out       = JsonValue::Object();
		auto& obj = std::get<JsonValue::ObjectType>(Out.Data);

		SkipWhitespace(C);
		if (C.Peek() == '}')
		{
			C.Next();
			return true;
		}

		while (true)
		{
			SkipWhitespace(C);
			if (C.Peek() != '"') return ParseError(C, "expected string key in object");

			std::string key;
			if (!ParseString(C, key)) return false;

			SkipWhitespace(C);
			if (C.Next() != ':') return ParseError(C, "expected ':' after object key");

			SkipWhitespace(C);
			JsonValue val;
			if (!ParseValue(C, val)) return false;

			obj.emplace_back(std::move(key), std::move(val));

			SkipWhitespace(C);
			char ch = C.Peek();
			if (ch == ',')
			{
				C.Next();
				continue;
			}
			if (ch == '}')
			{
				C.Next();
				return true;
			}
			return ParseError(C, "expected ',' or '}' in object");
		}
	}

	bool ParseArray(ParseCtx& C, JsonValue& Out)
	{
		C.Next(); // consume '['
		Out       = JsonValue::Array();
		auto& arr = std::get<JsonValue::ArrayType>(Out.Data);

		SkipWhitespace(C);
		if (C.Peek() == ']')
		{
			C.Next();
			return true;
		}

		while (true)
		{
			SkipWhitespace(C);
			JsonValue val;
			if (!ParseValue(C, val)) return false;
			arr.push_back(std::move(val));

			SkipWhitespace(C);
			char ch = C.Peek();
			if (ch == ',')
			{
				C.Next();
				continue;
			}
			if (ch == ']')
			{
				C.Next();
				return true;
			}
			return ParseError(C, "expected ',' or ']' in array");
		}
	}

	bool MatchLiteral(ParseCtx& C, std::string_view Expected)
	{
		if (C.Pos + Expected.size() > C.Text.size()) return false;
		if (C.Text.substr(C.Pos, Expected.size()) != Expected) return false;
		C.Pos += Expected.size();
		return true;
	}

	bool ParseValue(ParseCtx& C, JsonValue& Out)
	{
		SkipWhitespace(C);
		if (C.AtEnd()) return ParseError(C, "unexpected end of input");

		char ch = C.Peek();

		if (ch == '{') return ParseObject(C, Out);
		if (ch == '[') return ParseArray(C, Out);

		if (ch == '"')
		{
			std::string s;
			if (!ParseString(C, s)) return false;
			Out = JsonValue::String(std::move(s));
			return true;
		}

		if (ch == 't')
		{
			if (!MatchLiteral(C, "true")) return ParseError(C, "invalid literal (expected 'true')");
			Out = JsonValue::Bool(true);
			return true;
		}

		if (ch == 'f')
		{
			if (!MatchLiteral(C, "false")) return ParseError(C, "invalid literal (expected 'false')");
			Out = JsonValue::Bool(false);
			return true;
		}

		if (ch == 'n')
		{
			if (!MatchLiteral(C, "null")) return ParseError(C, "invalid literal (expected 'null')");
			Out = JsonValue::Null();
			return true;
		}

		if (ch == '-' || (ch >= '0' && ch <= '9'))
		{
			double num = 0.0;
			if (!ParseNumber(C, num)) return false;
			Out = JsonValue::Number(num);
			return true;
		}

		return ParseError(C, "unexpected character");
	}
} // anonymous namespace

JsonValue JsonParse(std::string_view Text)
{
	ParseCtx ctx{Text, 0};
	JsonValue result;
	if (!ParseValue(ctx, result)) return JsonValue::Null();
	return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Writer
// ═══════════════════════════════════════════════════════════════════════════

namespace
{
	void WriteIndent(std::string& Out, int Depth)
	{
		for (int i = 0; i < Depth; ++i) Out += '\t';
	}

	void WriteEscapedString(const std::string& Str, std::string& Out)
	{
		Out += '"';
		for (char ch : Str)
		{
			switch (ch)
			{
				case '"': Out += "\\\"";
					break;
				case '\\': Out += "\\\\";
					break;
				case '\b': Out += "\\b";
					break;
				case '\f': Out += "\\f";
					break;
				case '\n': Out += "\\n";
					break;
				case '\r': Out += "\\r";
					break;
				case '\t': Out += "\\t";
					break;
				default: if (static_cast<unsigned char>(ch) < 0x20)
					{
						char buf[8];
						snprintf(buf, sizeof(buf), "\\u%04x", ch);
						Out += buf;
					}
					else
					{
						Out += ch;
					}
					break;
			}
		}
		Out += '"';
	}

	void WriteNumber(double Val, std::string& Out)
	{
		char buf[64];
		if (Val == std::floor(Val) && std::fabs(Val) < 1e15) snprintf(buf, sizeof(buf), "%.1f", Val);
		else snprintf(buf, sizeof(buf), "%.17g", Val);
		Out += buf;
	}

	void WriteValue(const JsonValue& Val, std::string& Out, bool Pretty, int Depth);

	void WriteObject(const JsonValue::ObjectType& Obj, std::string& Out, bool Pretty, int Depth)
	{
		Out += '{';
		if (Obj.empty())
		{
			Out += '}';
			return;
		}

		for (size_t i = 0; i < Obj.size(); ++i)
		{
			if (Pretty)
			{
				Out += '\n';
				WriteIndent(Out, Depth + 1);
			}
			WriteEscapedString(Obj[i].first, Out);
			Out += Pretty ? ": " : ":";
			WriteValue(Obj[i].second, Out, Pretty, Depth + 1);
			if (i + 1 < Obj.size()) Out += ',';
		}

		if (Pretty)
		{
			Out += '\n';
			WriteIndent(Out, Depth);
		}
		Out += '}';
	}

	void WriteArray(const JsonValue::ArrayType& Arr, std::string& Out, bool Pretty, int Depth)
	{
		Out += '[';
		if (Arr.empty())
		{
			Out += ']';
			return;
		}

		// Compact single-line for arrays of only primitives
		bool allPrimitive = true;
		for (auto& v : Arr) if (v.IsObject() || v.IsArray())
		{
			allPrimitive = false;
			break;
		}

		if (allPrimitive && Pretty)
		{
			for (size_t i = 0; i < Arr.size(); ++i)
			{
				if (i > 0) Out += ", ";
				WriteValue(Arr[i], Out, false, 0);
			}
			Out += ']';
			return;
		}

		for (size_t i = 0; i < Arr.size(); ++i)
		{
			if (Pretty)
			{
				Out += '\n';
				WriteIndent(Out, Depth + 1);
			}
			WriteValue(Arr[i], Out, Pretty, Depth + 1);
			if (i + 1 < Arr.size()) Out += ',';
		}

		if (Pretty)
		{
			Out += '\n';
			WriteIndent(Out, Depth);
		}
		Out += ']';
	}

	void WriteValue(const JsonValue& Val, std::string& Out, bool Pretty, int Depth)
	{
		switch (Val.Type())
		{
			case JsonType::Null: Out += "null";
				break;
			case JsonType::Bool: Out += Val.AsBool() ? "true" : "false";
				break;
			case JsonType::Number: WriteNumber(Val.AsNumber(), Out);
				break;
			case JsonType::String: WriteEscapedString(Val.AsString(), Out);
				break;
			case JsonType::Array: WriteArray(Val.AsArray(), Out, Pretty, Depth);
				break;
			case JsonType::Object: WriteObject(Val.AsObject(), Out, Pretty, Depth);
				break;
		}
	}
} // anonymous namespace

std::string JsonWrite(const JsonValue& Val, bool Pretty)
{
	std::string out;
	out.reserve(4096);
	WriteValue(Val, out, Pretty, 0);
	if (Pretty) out += '\n';
	return out;
}