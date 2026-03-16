#pragma once

// Minimal JSON parser/writer for scene serialization (development tooling).
//
// This is an intentional choice over vendoring a full library (nlohmann/json,
// rapidjson, etc.). The engine's JSON needs are narrow — scene files and
// prefabs with known structure driven by runtime reflection. A ~400-line
// recursive descent parser keeps compile times low, has zero dependencies,
// and gives us full control over the format.
//
// Binary serialization replaces JSON for shipped games (Phase 9+), so this
// is strictly a development-time format. If requirements grow beyond what
// this handles, the free-function API (JsonParse/JsonWrite) can be
// reimplemented against a vendored library without changing call sites.

#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <cstdint>

enum class JsonType : uint8_t
{
	Null,
	Bool,
	Number,
	String,
	Array,
	Object
};

struct JsonValue
{
	using ObjectType = std::vector<std::pair<std::string, JsonValue>>;
	using ArrayType  = std::vector<JsonValue>;

	std::variant<std::monostate, bool, double, std::string, ArrayType, ObjectType> Data;

	// --- Type queries ---
	JsonType Type() const;
	bool IsNull() const;
	bool IsBool() const;
	bool IsNumber() const;
	bool IsString() const;
	bool IsArray() const;
	bool IsObject() const;

	// --- Const accessors (return defaults on type mismatch) ---
	bool AsBool(bool Default = false) const;
	double AsNumber(double Default = 0.0) const;
	float AsFloat(float Default = 0.0f) const;
	int AsInt(int Default = 0) const;
	const std::string& AsString() const;
	const ArrayType& AsArray() const;
	const ObjectType& AsObject() const;

	// --- Mutable accessors for building values ---
	ArrayType& GetArray();
	ObjectType& GetObject();

	// --- Object key lookup ---
	const JsonValue* Find(std::string_view Key) const;
	JsonValue* Find(std::string_view Key);

	// Insert-or-find for builder pattern. Only valid on Object values.
	JsonValue& operator[](const std::string& Key);

	// --- Factories ---
	static JsonValue Null();
	static JsonValue Bool(bool Val);
	static JsonValue Number(double Val);
	static JsonValue String(std::string Val);
	static JsonValue Array();
	static JsonValue Object();
};

// --- Free-function API ---
JsonValue JsonParse(std::string_view Text);
std::string JsonWrite(const JsonValue& Val, bool Pretty = true);