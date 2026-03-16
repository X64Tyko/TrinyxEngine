#include "EntityBuilder.h"

#include "Archetype.h"
#include "FieldMeta.h"
#include "Logger.h"
#include "Registry.h"
#include "Schema.h"
#include "TemporalFlags.h"

#include <cstring>
#include <fstream>
#include <sstream>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Write a numeric JSON value into a raw field pointer, dispatching on field size.
static void WriteFieldValue(void* dst, size_t fieldSize, const JsonValue& val)
{
	if (!val.IsNumber()) return;

	switch (fieldSize)
	{
		case 4:
			{
				float f = val.AsFloat();
				std::memcpy(dst, &f, 4);
				break;
			}
		case 8:
			{
				double d = val.AsNumber();
				std::memcpy(dst, &d, 8);
				break;
			}
		case 2:
			{
				auto v = static_cast<uint16_t>(val.AsInt());
				std::memcpy(dst, &v, 2);
				break;
			}
		case 1:
			{
				auto v = static_cast<uint8_t>(val.AsInt());
				std::memcpy(dst, &v, 1);
				break;
			}
		default:
			LOG_WARN_F("[EntityBuilder] Unsupported field size %zu, skipping", fieldSize);
			break;
	}
}

// Build a map of field name → (fieldArrayIndex, fieldSize) for an archetype.
struct FieldLookup
{
	size_t ArrayIndex;
	size_t Size;
};

static std::vector<std::pair<const char*, FieldLookup>> BuildFieldMap(const Archetype* arch)
{
	std::vector<std::pair<const char*, FieldLookup>> map;
	map.reserve(arch->CachedFieldArrayLayout.size());

	for (size_t i = 0; i < arch->CachedFieldArrayLayout.size(); ++i)
	{
		const char* name = arch->FieldArrayTemplateCache[i].debugName;
		if (name)
		{
			map.push_back({name, {i, arch->CachedFieldArrayLayout[i].Size}});
		}
	}
	return map;
}

// Find a field by name in the lookup.
static const FieldLookup* FindField(
	const std::vector<std::pair<const char*, FieldLookup>>& map,
	std::string_view name)
{
	for (const auto& [fieldName, lookup] : map)
	{
		if (fieldName && name == fieldName) return &lookup;
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// EntityBuilder
// ---------------------------------------------------------------------------

EntityID EntityBuilder::SpawnEntity(Registry* reg, const JsonValue& entityJson)
{
	// Read entity type name
	const JsonValue* typeVal = entityJson.Find("type");
	if (!typeVal || !typeVal->IsString())
	{
		LOG_ERROR("[EntityBuilder] Entity missing 'type' field");
		return EntityID{};
	}

	const std::string& typeName = typeVal->AsString();

	// Resolve type name → ClassID
	auto& MR        = MetaRegistry::Get();
	ClassID classID = MR.GetEntityByName(typeName);
	if (classID == 0)
	{
		LOG_ERROR_F("[EntityBuilder] Unknown entity type '%s'", typeName.c_str());
		return EntityID{};
	}

	// Create the entity
	std::vector<EntityID> ids = reg->CreateByClassID(classID, 1);
	if (ids.empty()) return EntityID{};

	EntityID id = ids[0];

	// Find the archetype for this entity type
	const auto& archetypes = reg->GetArchetypes();
	Archetype* arch        = nullptr;
	for (const auto& [key, a] : archetypes)
	{
		if (key.ID == classID)
		{
			arch = a;
			break;
		}
	}

	if (!arch || arch->Chunks.empty()) return id;

	// Build field name → array index map for this archetype
	auto fieldMap = BuildFieldMap(arch);

	// Find which chunk this entity landed in (last chunk, since we just pushed)
	size_t chunkIdx           = arch->Chunks.size() - 1;
	Chunk* chunk              = arch->Chunks[chunkIdx];
	uint32_t chunkEntityCount = arch->GetChunkCount(chunkIdx);
	uint32_t localIndex       = chunkEntityCount - 1; // Just-pushed entity is at the tail

	// Build field array table for the write frame
	void* fieldArrayTable[MAX_FIELD_ARRAYS];

	auto WriteEntity = [&]
	{
		// Zero-initialize all decomposed fields for this entity.
		// Fields omitted from JSON must not contain garbage — an unnormalized
		// quaternion (NaN/denorm) will crash Jolt's multiply operator.
		for (size_t i = 0; i < arch->CachedFieldArrayLayout.size(); ++i)
		{
			const auto& desc = arch->CachedFieldArrayLayout[i];
			if (!desc.isDecomposed || !fieldArrayTable[i]) continue;

			auto* base = static_cast<uint8_t*>(fieldArrayTable[i]);
			std::memset(base + localIndex * desc.Size, 0, desc.Size);
		}

		// Set the Active flag (first field is always TemporalFlags::Flags)
		const FieldLookup* flagsField = FindField(fieldMap, "Flags");
		if (flagsField)
		{
			auto* flagsArr       = static_cast<int32_t*>(fieldArrayTable[flagsField->ArrayIndex]);
			flagsArr[localIndex] = static_cast<int32_t>(TemporalFlagBits::Active);
		}

		// Apply component field values from JSON
		const JsonValue* components = entityJson.Find("components");
		if (!components || !components->IsObject()) return;

		for (const auto& [compName, compFields] : components->AsObject())
		{
			if (!compFields.IsObject()) continue;

			// Walk each field in this component's JSON
			for (const auto& [fieldName, fieldVal] : compFields.AsObject())
			{
				const FieldLookup* field = FindField(fieldMap, fieldName);
				if (!field)
				{
					LOG_WARN_F("[EntityBuilder] Unknown field '%s.%s' on entity type '%s'",
							   compName.c_str(), fieldName.c_str(), typeName.c_str());
					continue;
				}

				// Write the value into the field array at the entity's local index
				auto* base = static_cast<uint8_t*>(fieldArrayTable[field->ArrayIndex]);
				WriteFieldValue(base + localIndex * field->Size, field->Size, fieldVal);
			}
		}
	};

	arch->BuildFieldArrayTable(chunk, fieldArrayTable,
							   reg->GetTemporalCache()->GetActiveWriteFrame(),
							   reg->GetVolatileCache()->GetActiveWriteFrame());

	WriteEntity();

	return id;
}

size_t EntityBuilder::SpawnScene(Registry* reg, const JsonValue& sceneJson)
{
	const JsonValue* entities = sceneJson.Find("entities");
	if (!entities || !entities->IsArray())
	{
		LOG_ERROR("[EntityBuilder] Scene missing 'entities' array");
		return 0;
	}

	size_t count = 0;
	for (const auto& entityJson : entities->AsArray())
	{
		EntityID id = SpawnEntity(reg, entityJson);
		if (id.IsValid()) ++count;
	}

	const JsonValue* nameVal = sceneJson.Find("name");
	const char* sceneName    = (nameVal && nameVal->IsString()) ? nameVal->AsString().c_str() : "unnamed";
	LOG_INFO_F("[EntityBuilder] Spawned %zu entities from scene '%s'", count, sceneName);

	return count;
}

size_t EntityBuilder::SpawnFromFile(Registry* reg, const char* filePath)
{
	std::ifstream file(filePath);
	if (!file.is_open())
	{
		LOG_ERROR_F("[EntityBuilder] Failed to open file '%s'", filePath);
		return 0;
	}

	std::ostringstream ss;
	ss << file.rdbuf();
	std::string content = ss.str();

	JsonValue root = JsonParse(content);
	if (root.IsNull())
	{
		LOG_ERROR_F("[EntityBuilder] Failed to parse JSON from '%s'", filePath);
		return 0;
	}

	// Detect format: prefab (has "type" at root) vs scene (has "entities" array)
	if (root.Find("entities"))
	{
		return SpawnScene(reg, root);
	}
	else if (root.Find("type"))
	{
		EntityID id = SpawnEntity(reg, root);
		return id.IsValid() ? 1 : 0;
	}

	LOG_ERROR_F("[EntityBuilder] Unrecognized JSON format in '%s'", filePath);
	return 0;
}

// ---------------------------------------------------------------------------
// Save helpers
// ---------------------------------------------------------------------------

// Read a numeric value from a raw field pointer and return it as a JsonValue.
static JsonValue ReadFieldValue(const void* src, size_t fieldSize)
{
	switch (fieldSize)
	{
		case 4:
			{
				float f;
				std::memcpy(&f, src, 4);
				return JsonValue::Number(static_cast<double>(f));
			}
		case 8:
			{
				double d;
				std::memcpy(&d, src, 8);
				return JsonValue::Number(d);
			}
		case 2:
			{
				uint16_t v;
				std::memcpy(&v, src, 2);
				return JsonValue::Number(static_cast<double>(v));
			}
		case 1:
			{
				uint8_t v;
				std::memcpy(&v, src, 1);
				return JsonValue::Number(static_cast<double>(v));
			}
		default: return JsonValue::Number(0.0);
	}
}

// ---------------------------------------------------------------------------
// EntityBuilder — Save
// ---------------------------------------------------------------------------

JsonValue EntityBuilder::SerializeEntity(Registry* reg, Archetype* arch, size_t chunkIdx, uint32_t localIndex)
{
	JsonValue entity = JsonValue::Object();

	// Entity type name
	const auto& MR       = MetaRegistry::Get();
	const char* typeName = MR.EntityGetters[arch->ArchClassID].Name;
	entity["type"]       = JsonValue::String(typeName ? typeName : "unknown");

	// Build field array table for reading
	Chunk* chunk = arch->Chunks[chunkIdx];
	void* fieldArrayTable[MAX_FIELD_ARRAYS];
	arch->BuildFieldArrayTable(chunk, fieldArrayTable,
							   reg->GetTemporalCache()->GetActiveWriteFrame(),
							   reg->GetVolatileCache()->GetActiveWriteFrame());

	// Group fields by component
	const auto& CFR      = ComponentFieldRegistry::Get();
	JsonValue components = JsonValue::Object();

	// Track which component we're building — fields are contiguous per component
	ComponentTypeID currentCompID = 0;
	JsonValue* currentComp        = nullptr;

	for (size_t i = 0; i < arch->CachedFieldArrayLayout.size(); ++i)
	{
		const auto& desc = arch->CachedFieldArrayLayout[i];
		if (!desc.isDecomposed || !fieldArrayTable[i]) continue;

		// Skip the Flags field (index 0 by convention, componentID for TemporalFlags)
		const char* fieldName = arch->FieldArrayTemplateCache[i].debugName;
		if (!fieldName) continue;

		// Look up component name when the component ID changes
		if (desc.componentID != currentCompID)
		{
			currentCompID        = desc.componentID;
			const char* compName = CFR.GetAllComponents().count(currentCompID)
									   ? CFR.GetComponentMeta(currentCompID).Name
									   : nullptr;

			if (compName)
			{
				components[compName] = JsonValue::Object();
				currentComp          = components.Find(compName);
			}
			else
			{
				currentComp = nullptr;
			}
		}

		if (!currentComp) continue;

		// Read field value
		const auto* base          = static_cast<const uint8_t*>(fieldArrayTable[i]);
		(*currentComp)[fieldName] = ReadFieldValue(base + localIndex * desc.Size, desc.Size);
	}

	entity["components"] = std::move(components);
	return entity;
}

JsonValue EntityBuilder::SerializeScene(Registry* reg, const char* sceneName)
{
	JsonValue scene = JsonValue::Object();
	scene["name"]   = JsonValue::String(sceneName);

	JsonValue entities = JsonValue::Array();

	// Build field array table to check Active flags
	const auto& archetypes = reg->GetArchetypes();

	for (const auto& [key, arch] : archetypes)
	{
		for (size_t ci = 0; ci < arch->Chunks.size(); ++ci)
		{
			uint32_t count = arch->GetChunkCount(ci);

			// Build field table to read flags
			void* fieldArrayTable[MAX_FIELD_ARRAYS];
			arch->BuildFieldArrayTable(arch->Chunks[ci], fieldArrayTable,
									   reg->GetTemporalCache()->GetActiveWriteFrame(),
									   reg->GetVolatileCache()->GetActiveWriteFrame());

			// Flags are at field index 0 by convention
			const auto* flagsArr = (arch->CachedFieldArrayLayout.size() > 0 && fieldArrayTable[0])
									   ? static_cast<const int32_t*>(fieldArrayTable[0])
									   : nullptr;

			for (uint32_t li = 0; li < count; ++li)
			{
				// Skip inactive entities
				if (flagsArr && !(flagsArr[li] & static_cast<int32_t>(TemporalFlagBits::Active))) continue;

				entities.GetArray().push_back(SerializeEntity(reg, arch, ci, li));
			}
		}
	}

	size_t entityCount = entities.GetArray().size();
	scene["entities"]  = std::move(entities);

	LOG_INFO_F("[EntityBuilder] Serialized %zu entities into scene '%s'", entityCount, sceneName);
	return scene;
}

bool EntityBuilder::SaveToFile(Registry* reg, const char* sceneName, const char* filePath)
{
	JsonValue scene  = SerializeScene(reg, sceneName);
	std::string json = JsonWrite(scene, true);

	std::ofstream file(filePath);
	if (!file.is_open())
	{
		LOG_ERROR_F("[EntityBuilder] Failed to open file '%s' for writing", filePath);
		return false;
	}

	file << json;
	file.close();

	LOG_INFO_F("[EntityBuilder] Saved scene to '%s'", filePath);
	return true;
}
