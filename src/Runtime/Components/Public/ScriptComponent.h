#pragma once
#include <cstdint>
#include "ComponentView.h" // SystemID, ComponentTypeID
#include "FieldMeta.h"     // FieldValueType

// Runtime-defined component schema — defined at runtime via ScriptRegistry.
// Intended for early-project iteration: create components in the editor
// without recompiling. When the schema is stable, use ComponentGeneratorPanel
// to export a compile-time header and migrate to a ConstructView.
//
// Blob storage is AoS (one packed byte buffer per entity). There is no SIMD
// sweep path. NodeScript accesses fields via ScriptRegistry::Get{Float,Int32,...}.
//
// Thread safety: schema mutation (RegisterComponent, AddField) must happen
// outside the Logic thread hot path — during handshake or while PIE is stopped.
// Field reads/writes are expected from the Logic thread only.

// ──────────────────────────────────────────────────────────────────────────────
// Types

static constexpr uint8_t kMaxScriptFields     = 32;
static constexpr uint8_t kMaxScriptComponents = 64;

enum class ScriptFieldType : uint8_t
{
	Float   = 0, // float / SimFloat (4 bytes)
	Int32   = 1, // int32_t          (4 bytes)
	UInt32  = 2, // uint32_t         (4 bytes)
	Bool    = 3, // bool stored as uint8 (1 byte, padded to 4 at end of blob)
	Fixed32 = 4, // Fixed32 integer  (4 bytes)
};

static constexpr uint8_t ScriptFieldByteSizes[] = { 4, 4, 4, 1, 4 };

inline FieldValueType ScriptFieldToValueType(ScriptFieldType t)
{
	switch (t)
	{
		case ScriptFieldType::Float: return FieldValueType::Float32;
		case ScriptFieldType::Int32: return FieldValueType::Int32;
		case ScriptFieldType::UInt32: return FieldValueType::Uint32;
		case ScriptFieldType::Bool: return FieldValueType::Int32; // treat bool as int for editor
		case ScriptFieldType::Fixed32: return FieldValueType::Fixed32;
		default: return FieldValueType::Unknown;
	}
}

struct ScriptFieldDesc
{
	char Name[64]        = {};
	ScriptFieldType Type = ScriptFieldType::Float;
	uint16_t ByteOffset  = 0; // byte offset within the per-entity blob
	uint16_t ByteSize    = 4; // byte size (derived from Type at AddField time)
};

// ──────────────────────────────────────────────────────────────────────────────
// Schema (stored in ScriptRegistry, pointer-stable — see ScriptRegistry.h)

struct ScriptSchema
{
	char Name[64]                            = {};
	ComponentTypeID TypeID                   = 0;
	SystemID SysID                           = SystemID::None;
	CacheTier Tier                           = CacheTier::Volatile;
	uint16_t BlobSize                        = 0;
	uint8_t FieldCount                       = 0;
	ScriptFieldDesc Fields[kMaxScriptFields] = {};
};
