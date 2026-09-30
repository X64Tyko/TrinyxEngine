#include "ScriptRegistry.h"

#include "AssetTypes.h"
#include "FieldMeta.h"
#include "Logger.h"
#include "ReflectionRegistry.h"
#include "Types.h"

#include <cassert>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// ScriptComponentStorage

void ScriptComponentStorage::Initialize(uint32_t capacity)
{
	Capacity = capacity;
	HasComp  = std::make_unique<uint8_t[]>(capacity);
	memset(HasComp.get(), 0, capacity);

	if (Schema.BlobSize > 0)
	{
		Blobs = std::make_unique<uint8_t[]>((size_t)Schema.BlobSize * capacity);
		memset(Blobs.get(), 0, (size_t)Schema.BlobSize * capacity);
	}
}

void ScriptComponentStorage::GrowBlobs(uint16_t oldBlobSize, uint16_t newBlobSize)
{
	assert(newBlobSize > oldBlobSize);
	if (Capacity == 0) return;

	auto grown = std::make_unique<uint8_t[]>((size_t)newBlobSize * Capacity);
	memset(grown.get(), 0, (size_t)newBlobSize * Capacity);

	if (oldBlobSize > 0 && Blobs)
	{
		for (uint32_t i = 0; i < Capacity; ++i)
		{
			memcpy(grown.get() + (size_t)i * newBlobSize,
				Blobs.get() + (size_t)i * oldBlobSize,
				oldBlobSize);
		}
	}

	Blobs = std::move(grown);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers

namespace
{
// Build a FieldMeta list from the current schema for ReflectionRegistry.
// Name pointers remain valid because ScriptComponentStorage lives in a fixed array.
std::vector<FieldMeta> BuildFieldMetas(const ScriptSchema& schema)
{
	std::vector<FieldMeta> metas;
	metas.reserve(schema.FieldCount);
	for (uint8_t i = 0; i < schema.FieldCount; ++i)
	{
		const ScriptFieldDesc& f = schema.Fields[i];
		metas.push_back(FieldMeta{
			f.ByteSize,   // Size
			4,            // Alignment
			f.ByteOffset, // OffsetInStruct
			0,            // OffsetInChunk (unused for script)
			f.Name,
			ScriptFieldToValueType(f.Type),
			AssetType::Invalid,
		});
	}
	return metas;
}

void SyncToReflection(const ScriptSchema& schema)
{
	// CacheTier::Temporal script components are Volatile-equivalent — no rollback.
	CacheTier reflected = (schema.Tier == CacheTier::Temporal) ? CacheTier::Volatile : schema.Tier;
	ReflectionRegistry::Get().RegisterFields(
		schema.TypeID, schema.Name,
		BuildFieldMetas(schema),
		reflected,
		0xFF); // 0xFF = no SoA slab slot
}

template <typename T>
T ReadBlob(const ScriptComponentStorage* s, uint32_t cacheIdx, uint16_t offset)
{
	assert(s && s->HasComp && s->HasComp[cacheIdx] && s->Blobs);
	T val;
	memcpy(&val, s->Blobs.get() + (size_t)cacheIdx * s->Schema.BlobSize + offset, sizeof(T));
	return val;
}

template <typename T>
void WriteBlob(ScriptComponentStorage* s, uint32_t cacheIdx, uint16_t offset, T val)
{
	assert(s && s->HasComp && s->HasComp[cacheIdx] && s->Blobs);
	memcpy(s->Blobs.get() + (size_t)cacheIdx * s->Schema.BlobSize + offset, &val, sizeof(T));
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ScriptRegistry

ScriptRegistry& ScriptRegistry::Get()
{
	static ScriptRegistry instance;
	return instance;
}

void ScriptRegistry::Initialize(uint32_t maxCachedEntities)
{
	MaxEntities = maxCachedEntities;
	for (uint8_t i = 0; i < ComponentCount; ++i)
		Components[i].Initialize(maxCachedEntities);
}

void ScriptRegistry::Shutdown()
{
	for (uint8_t i = 0; i < ComponentCount; ++i)
	{
		Components[i].Blobs.reset();
		Components[i].HasComp.reset();
		Components[i].Capacity = 0;
		Components[i].Schema   = ScriptSchema{};
	}
	NameToIndex.clear();
	IDToIndex.clear();
	ComponentCount = 0;
	MaxEntities    = 0;
}

// ── Schema management ─────────────────────────────────────────────────────────

ComponentTypeID ScriptRegistry::RegisterComponent(const char* name, SystemID sysID, CacheTier tier)
{
	if (NameToIndex.count(name))
		return Components[NameToIndex.at(name)].Schema.TypeID;

	if (ComponentCount >= kMaxScriptComponents)
	{
		LOG_ENG_ERROR_F("[ScriptRegistry] kMaxScriptComponents (%u) reached", kMaxScriptComponents);
		return 0;
	}
	if (Internal::g_GlobalComponentCounter >= MAX_COMPONENTS)
	{
		LOG_ENG_ERROR("[ScriptRegistry] MAX_COMPONENTS exceeded — cannot register more components");
		return 0;
	}

	uint8_t idx               = ComponentCount++;
	ScriptComponentStorage& s = Components[idx];

	strncpy(s.Schema.Name, name, sizeof(s.Schema.Name) - 1);
	s.Schema.TypeID     = Internal::g_GlobalComponentCounter++;
	s.Schema.SysID      = sysID;
	s.Schema.Tier       = tier;
	s.Schema.BlobSize   = 0;
	s.Schema.FieldCount = 0;

	NameToIndex[name]          = idx;
	IDToIndex[s.Schema.TypeID] = idx;

	SyncToReflection(s.Schema);

	if (MaxEntities > 0)
		s.Initialize(MaxEntities);

	LOG_ENG_INFO_F("[ScriptRegistry] Registered '%s' TypeID=%u [S=AoS,no-SIMD,no-rollback]",
		name, s.Schema.TypeID);
	return s.Schema.TypeID;
}

bool ScriptRegistry::AddField(ComponentTypeID id, const char* fieldName, ScriptFieldType type)
{
	ScriptComponentStorage* s = FindStorage(id);
	if (!s)
	{
		LOG_ENG_ERROR_F("[ScriptRegistry] AddField: unknown TypeID %u", id);
		return false;
	}
	if (s->Schema.FieldCount >= kMaxScriptFields)
	{
		LOG_ENG_ERROR_F("[ScriptRegistry] AddField: field limit (%u) reached on '%s'",
			kMaxScriptFields, s->Schema.Name);
		return false;
	}
	// Reject duplicate field names.
	for (uint8_t i = 0; i < s->Schema.FieldCount; ++i)
	{
		if (strcmp(s->Schema.Fields[i].Name, fieldName) == 0)
		{
			LOG_ENG_WARN_F("[ScriptRegistry] AddField: field '%s' already exists on '%s'",
				fieldName, s->Schema.Name);
			return false;
		}
	}

	uint8_t idx        = s->Schema.FieldCount++;
	ScriptFieldDesc& f = s->Schema.Fields[idx];
	strncpy(f.Name, fieldName, sizeof(f.Name) - 1);
	f.Type       = type;
	f.ByteSize   = ScriptFieldByteSizes[static_cast<uint8_t>(type)];
	f.ByteOffset = s->Schema.BlobSize;

	uint16_t newBlobSize = s->Schema.BlobSize + f.ByteSize;

	if (s->Blobs)
		s->GrowBlobs(s->Schema.BlobSize, newBlobSize);
	else if (MaxEntities > 0 && !s->HasComp)
		s->Initialize(MaxEntities);

	s->Schema.BlobSize = newBlobSize;

	SyncToReflection(s->Schema);
	return true;
}

ComponentTypeID ScriptRegistry::RegisterOrUpdate(
	const char* name, SystemID sysID, CacheTier tier,
	const ScriptFieldType* fieldTypes, const char* const* fieldNames,
	uint8_t fieldCount, bool* bBreakingChange)
{
	if (bBreakingChange) *bBreakingChange = false;

	auto it = NameToIndex.find(name);
	if (it != NameToIndex.end())
	{
		ScriptComponentStorage& s = Components[it->second];

		// Validate all existing fields are still present in the same position with the same type.
		const uint8_t existingCount = s.Schema.FieldCount;
		if (fieldCount < existingCount)
		{
			if (bBreakingChange) *bBreakingChange = true;
			return s.Schema.TypeID;
		}
		for (uint8_t i = 0; i < existingCount; ++i)
		{
			if (strcmp(s.Schema.Fields[i].Name, fieldNames[i]) != 0 || s.Schema.Fields[i].Type != fieldTypes[i])
			{
				if (bBreakingChange) *bBreakingChange = true;
				return s.Schema.TypeID;
			}
		}

		// Additive: append new trailing fields.
		for (uint8_t i = existingCount; i < fieldCount; ++i)
			AddField(s.Schema.TypeID, fieldNames[i], fieldTypes[i]);

		return s.Schema.TypeID;
	}

	// New schema.
	ComponentTypeID id = RegisterComponent(name, sysID, tier);
	if (id == 0) return 0;

	for (uint8_t i = 0; i < fieldCount; ++i)
		AddField(id, fieldNames[i], fieldTypes[i]);

	return id;
}

// ── Accessors ────────────────────────────────────────────────────────────────

const ScriptSchema* ScriptRegistry::GetSchema(ComponentTypeID id) const
{
	const ScriptComponentStorage* s = FindStorage(id);
	return s ? &s->Schema : nullptr;
}

const ScriptSchema* ScriptRegistry::GetSchema(const char* name) const
{
	auto it = NameToIndex.find(name);
	return (it != NameToIndex.end()) ? &Components[it->second].Schema : nullptr;
}

const ScriptComponentStorage* ScriptRegistry::GetStorage(ComponentTypeID id) const
{
	return FindStorage(id);
}

void ScriptRegistry::ForEachSchema(
	const std::function<void(const ScriptSchema&, const ScriptComponentStorage&)>& fn) const
{
	for (uint8_t i = 0; i < ComponentCount; ++i)
		fn(Components[i].Schema, Components[i]);
}

// ── Entity attachment ─────────────────────────────────────────────────────────

void ScriptRegistry::Attach(uint32_t cacheIdx, ComponentTypeID id)
{
	ScriptComponentStorage* s = FindStorage(id);
	if (!s) return;

	// Lazy blob allocation (component registered before Initialize was called).
	if (!s->HasComp)
	{
		const uint32_t cap = (MaxEntities > 0) ? MaxEntities : 65536u;
		s->Initialize(cap);
	}

	if (cacheIdx >= s->Capacity) return;

	s->HasComp[cacheIdx] = 1;
	if (s->Blobs && s->Schema.BlobSize > 0)
		memset(s->Blobs.get() + (size_t)cacheIdx * s->Schema.BlobSize, 0, s->Schema.BlobSize);
}

void ScriptRegistry::Detach(uint32_t cacheIdx, ComponentTypeID id)
{
	ScriptComponentStorage* s = FindStorage(id);
	if (!s || !s->HasComp || cacheIdx >= s->Capacity) return;
	s->HasComp[cacheIdx] = 0;
}

bool ScriptRegistry::HasComponent(uint32_t cacheIdx, ComponentTypeID id) const
{
	const ScriptComponentStorage* s = FindStorage(id);
	if (!s || !s->HasComp || cacheIdx >= s->Capacity) return false;
	return s->HasComp[cacheIdx] != 0;
}

void ScriptRegistry::OnEntityMoved(uint32_t oldIndex, uint32_t newIndex)
{
	for (uint8_t i = 0; i < ComponentCount; ++i)
	{
		ScriptComponentStorage& s = Components[i];
		if (!s.HasComp) continue;
		if (oldIndex >= s.Capacity || newIndex >= s.Capacity) continue;

		if (s.HasComp[oldIndex])
		{
			s.HasComp[newIndex] = 1;
			s.HasComp[oldIndex] = 0;
			if (s.Blobs && s.Schema.BlobSize > 0)
			{
				memcpy(s.Blobs.get() + (size_t)newIndex * s.Schema.BlobSize,
					s.Blobs.get() + (size_t)oldIndex * s.Schema.BlobSize,
					s.Schema.BlobSize);
			}
		}
	}
}

// ── Typed field access ────────────────────────────────────────────────────────

float ScriptRegistry::GetFloat(uint32_t ci, ComponentTypeID id, uint16_t off) const
{
	return ReadBlob<float>(FindStorage(id), ci, off);
}
void ScriptRegistry::SetFloat(uint32_t ci, ComponentTypeID id, uint16_t off, float v)
{
	WriteBlob(FindStorage(id), ci, off, v);
}
int32_t ScriptRegistry::GetInt32(uint32_t ci, ComponentTypeID id, uint16_t off) const
{
	return ReadBlob<int32_t>(FindStorage(id), ci, off);
}
void ScriptRegistry::SetInt32(uint32_t ci, ComponentTypeID id, uint16_t off, int32_t v)
{
	WriteBlob(FindStorage(id), ci, off, v);
}
uint32_t ScriptRegistry::GetUInt32(uint32_t ci, ComponentTypeID id, uint16_t off) const
{
	return ReadBlob<uint32_t>(FindStorage(id), ci, off);
}
void ScriptRegistry::SetUInt32(uint32_t ci, ComponentTypeID id, uint16_t off, uint32_t v)
{
	WriteBlob(FindStorage(id), ci, off, v);
}
bool ScriptRegistry::GetBool(uint32_t ci, ComponentTypeID id, uint16_t off) const
{
	return ReadBlob<uint8_t>(FindStorage(id), ci, off) != 0;
}
void ScriptRegistry::SetBool(uint32_t ci, ComponentTypeID id, uint16_t off, bool v)
{
	uint8_t b = v ? 1 : 0;
	WriteBlob(FindStorage(id), ci, off, b);
}

const ScriptFieldDesc* ScriptRegistry::FindField(ComponentTypeID id, const char* fieldName) const
{
	const ScriptComponentStorage* s = FindStorage(id);
	if (!s) return nullptr;
	for (uint8_t i = 0; i < s->Schema.FieldCount; ++i)
		if (strcmp(s->Schema.Fields[i].Name, fieldName) == 0)
			return &s->Schema.Fields[i];
	return nullptr;
}

// ── Private helpers ───────────────────────────────────────────────────────────

ScriptComponentStorage* ScriptRegistry::FindStorage(ComponentTypeID id)
{
	auto it = IDToIndex.find(id);
	return (it != IDToIndex.end()) ? &Components[it->second] : nullptr;
}

const ScriptComponentStorage* ScriptRegistry::FindStorage(ComponentTypeID id) const
{
	auto it = IDToIndex.find(id);
	return (it != IDToIndex.end()) ? &Components[it->second] : nullptr;
}
