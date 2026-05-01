#pragma once
#include <memory>
#include <vector>
#include <string>
#include <cstring>
#include "Json.h"
#include "Types.h"
#include "Archetype.h"
#include "CacheSlotMeta.h"
#include "ReflectionRegistry.h"
#include "Registry.h"
#include "FieldMeta.h"
#include "TemporalComponentCache.h" // for GetWriteFramePtr

class UndoCommand
{
public:
	virtual ~UndoCommand() = default;
	virtual void Execute() = 0; // redo
	virtual void Undo() = 0;
	virtual bool MergeWith(const UndoCommand&) { return false; }
};

// --- Serialization helpers ---

JsonValue SerializeEntityFields(Registry* reg, Archetype* arch, Chunk* chunk, uint16_t localIndex);
void DeserializeEntityFields(Registry* reg, Archetype* arch, Chunk* chunk, uint16_t localIndex, const JsonValue& state);
void MarkEntityDirty(Registry* reg, Archetype* arch, Chunk* chunk, uint16_t localIndex);
void* GetFieldPtr(Archetype* arch, Chunk* chunk, const char* fieldName, uint16_t localIndex, Registry* reg);

// --- Concrete commands ---

class EntityTransformCommand : public UndoCommand
{
public:
	EntityTransformCommand(Archetype* arch, Chunk* chunk, uint16_t localIndex, Registry* reg)
		: m_Arch(arch)
		, m_Chunk(chunk)
		, m_LocalIndex(localIndex)
		, m_Reg(reg)
	{
		m_Before = SerializeEntityFields(reg, arch, chunk, localIndex);
	}

	void SetAfter(const JsonValue& after) { m_After = after; }
	void Execute() override;
	void Undo() override;
	bool MergeWith(const UndoCommand& other) override;

private:
	Archetype* m_Arch;
	Chunk* m_Chunk;
	uint16_t m_LocalIndex;
	Registry* m_Reg;
	JsonValue m_Before;
	JsonValue m_After;
};

// Command for property panel edits (single field)
class ComponentFieldChangeCommand : public UndoCommand
{
public:
	ComponentFieldChangeCommand(Archetype* arch, Chunk* chunk, uint16_t localIndex,
								const char* fieldName, Registry* reg,
								const void* oldValue, FieldValueType type, size_t fieldSize)
		: m_Arch(arch)
		, m_Chunk(chunk)
		, m_LocalIndex(localIndex)
		, m_FieldName(fieldName)
		, m_Reg(reg)
		, m_Type(type)
		, m_FieldSize(fieldSize)
	{
		m_OldValue.resize(fieldSize);
		std::memcpy(m_OldValue.data(), oldValue, fieldSize);
	}

	void SetNewValue(const void* newValue)
	{
		m_NewValue.resize(m_FieldSize);
		std::memcpy(m_NewValue.data(), newValue, m_FieldSize);
	}

	void Execute() override;
	void Undo() override;
	bool MergeWith(const UndoCommand& other) override;

private:
	Archetype* m_Arch;
	Chunk* m_Chunk;
	uint16_t m_LocalIndex;
	std::string m_FieldName;
	Registry* m_Reg;
	FieldValueType m_Type;
	size_t m_FieldSize;
	std::vector<uint8_t> m_OldValue;
	std::vector<uint8_t> m_NewValue;
};
