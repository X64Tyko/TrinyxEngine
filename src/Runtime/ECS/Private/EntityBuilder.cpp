#include "EntityBuilder.h"

#include "Archetype.h"
#include "AssetRegistry.h"
#include "FieldMeta.h"
#include "Logger.h"
#include "Registry.h"
#include "Schema.h"
#include "CacheSlotMeta.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Write a numeric JSON value into a raw field pointer, using FieldValueType for
// correct interpretation. Without this, uint32 fields (MeshID, Shape, Motion)
// get written as float bit patterns, producing garbage values on the GPU.
static void WriteFieldValue(void* dst, size_t fieldSize, FieldValueType valueType, const JsonValue& val)
{
	if (!val.IsNumber()) return;

	switch (valueType)
	{
		case FieldValueType::Fixed32:
			{
				SimFloat f = SimFloat(val.AsFloat());
				std::memcpy(dst, &f, 4);
				break;
			}
		case FieldValueType::Float32:
			{
				SimFloat f = SimFloat(val.AsFloat());
				std::memcpy(dst, &f, 4);
				break;
			}
		case FieldValueType::Float64:
			{
				double d = val.AsNumber();
				std::memcpy(dst, &d, 8);
				break;
			}
		case FieldValueType::Int32:
			{
				auto v = static_cast<int32_t>(val.AsInt());
				std::memcpy(dst, &v, 4);
				break;
			}
		case FieldValueType::Uint32:
			{
				auto v = static_cast<uint32_t>(val.AsInt());
				std::memcpy(dst, &v, 4);
				break;
			}
		case FieldValueType::Int64:
			{
				auto v = static_cast<int64_t>(val.AsNumber());
				std::memcpy(dst, &v, 8);
				break;
			}
		case FieldValueType::Uint64:
			{
				auto v = static_cast<uint64_t>(val.AsNumber());
				std::memcpy(dst, &v, 8);
				break;
			}
		default:
			switch (fieldSize)
			{
				case 4:
					{
						SimFloat f = SimFloat(val.AsFloat());
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
				default: LOG_ENG_WARN_F("[EntityBuilder] Unsupported field size %zu, skipping", fieldSize);
					break;
			}
			break;
	}
}

// Build a map of field name → (fieldSlotIndex, fieldSize, valueType, refAssetType) for an
// archetype using ArchetypeFieldLayout + ComponentFieldRegistry for name resolution.
struct FieldLookup
{
	size_t ArrayIndex;
	size_t Size;
	FieldValueType ValueType;
	AssetType RefAssetType = AssetType::Invalid; // Non-Invalid = asset reference field
};

static std::vector<std::pair<const char*, FieldLookup>> BuildFieldMap(const Archetype* arch)
{
	std::vector<std::pair<const char*, FieldLookup>> map;
	const auto& cfr = ReflectionRegistry::Get();

	for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
	{
		const auto* fields = cfr.GetFields(fdesc.componentID);
		const char* name   = (fields && fdesc.componentSlotIndex < fields->size())
							   ? (*fields)[fdesc.componentSlotIndex].Name
							   : nullptr;
		if (name)
		{
			map.push_back({name, {fdesc.fieldSlotIndex, fdesc.fieldSize, fdesc.valueType, fdesc.refAssetType}});
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

// Thread-local cycle guard — stores absolute paths of prefabs currently being loaded.
static thread_local std::unordered_set<std::string> ActivePrefabLoads;

// Thread-local handle collector — non-null only during SpawnFromFileTracked.
static thread_local std::vector<GlobalEntityHandle>* ActiveHandleCollector = nullptr;

EntityHandle EntityBuilder::SpawnEntity(Registry* reg, const JsonValue& entityJson, bool bBackground)
{
	// Read entity type name
	const JsonValue* typeVal = entityJson.Find("type");
	if (!typeVal || !typeVal->IsString())
	{
		LOG_ENG_ERROR("[EntityBuilder] Entity missing 'type' field");
		return EntityHandle{};
	}

	const std::string& typeName = typeVal->AsString();

	const uint32_t activeMask = bBackground ? 0u : static_cast<uint32_t>(TemporalFlagBits::Active);
	const int32_t spawnFlags  = static_cast<int32_t>(activeMask | static_cast<uint32_t>(TemporalFlagBits::Alive));

	// Resolve type name → ClassID
	auto& MR        = ReflectionRegistry::Get();
	ClassID classID = MR.GetEntityByName(typeName);
	if (classID == 0)
	{
		LOG_ENG_ERROR_F("[EntityBuilder] Unknown entity type '%s'", typeName.c_str());
		return EntityHandle{};
	}

	EntityHandle lHandle = reg->CreateByClassID(classID, [&](EntityRecord& record, void** fieldArrayTable)
	{
		auto fieldMap       = BuildFieldMap(record.Arch);
		uint32_t localIndex = record.LocalIndex;

		// Zero-initialize all fields for this entity.
		for (const auto& [fkey, fdesc] : record.Arch->ArchetypeFieldLayout)
		{
			size_t idx = fdesc.fieldSlotIndex;
			if (!fieldArrayTable[idx]) continue;
			auto* base = static_cast<uint8_t*>(fieldArrayTable[idx]);
			std::memset(base + localIndex * fdesc.fieldSize, 0, fdesc.fieldSize);
		}

		// Apply component field values from JSON
		const JsonValue* components = entityJson.Find("components");
		if (components && components->IsObject())
		{
			for (const auto& [compName, compFields] : components->AsObject())
			{
				if (!compFields.IsObject()) continue;

				for (const auto& [fieldName, fieldVal] : compFields.AsObject())
				{
					const FieldLookup* field = FindField(fieldMap, fieldName);
					if (!field)
					{
						LOG_ENG_WARN_F("[EntityBuilder] Unknown field '%s.%s' on entity type '%s'",
									   compName.c_str(), fieldName.c_str(), typeName.c_str());
						continue;
					}

					// Asset ref fields: string value → RegisterPendingCheckout,
					// number value → raw write (legacy scene files store raw slot indices).
					if (field->RefAssetType != AssetType::Invalid && fieldVal.IsString())
					{
						auto* fieldPtr = static_cast<uint32_t*>(fieldArrayTable[field->ArrayIndex]) + localIndex;
						AssetRegistry::RegisterPendingCheckout(fieldPtr, TnxName(fieldVal.AsString().c_str()));
					}
					else
					{
						auto* base = static_cast<uint8_t*>(fieldArrayTable[field->ArrayIndex]);
						WriteFieldValue(base + localIndex * field->Size, field->Size, field->ValueType, fieldVal);
					}
				}
			}
		}

		// Set flags AFTER JSON fields — existing scenes stored Flags as float
		// (-0.0 = 0x80000000) which breaks under type-aware deserialization.
		const FieldLookup* flagsField = FindField(fieldMap, "Flags");
		if (flagsField)
		{
			auto* flagsArr       = static_cast<int32_t*>(fieldArrayTable[flagsField->ArrayIndex]);
			flagsArr[localIndex] = spawnFlags;
		}
	});

	if (lHandle.IsValid() && ActiveHandleCollector) ActiveHandleCollector->push_back(reg->GlobalEntityRegistry.LookupGlobalHandle(lHandle));

	return lHandle;
}
// ---------------------------------------------------------------------------
// Prefab loading helpers
// ---------------------------------------------------------------------------

// Load and parse an asset JSON file from an absolute path.
static JsonValue LoadAssetJSON(const std::string& path)
{
	std::ifstream file(path);
	if (!file.is_open())
	{
		LOG_ENG_ERROR_F("[EntityBuilder] Failed to open asset '%s'", path.c_str());
		return JsonValue{};
	}
	std::ostringstream ss;
	ss << file.rdbuf();
	return JsonParse(ss.str());
}

// Deep-merge override component fields on top of an entity JSON in place.
// overrides is a "components" object: { "CTransform": { "PosX": 5.0 }, ... }
static void MergeComponentOverrides(JsonValue& entityJson, const JsonValue* overrides)
{
	if (!overrides || !overrides->IsObject()) return;
	JsonValue* baseComponents = entityJson.Find("components");
	if (!baseComponents || !baseComponents->IsObject()) return;

	for (const auto& [compName, compOverride] : overrides->AsObject())
	{
		if (!compOverride.IsObject()) continue;

		JsonValue* baseComp = baseComponents->Find(compName);
		if (!baseComp)
		{
			(*baseComponents)[compName] = JsonValue::Object();
			baseComp                    = baseComponents->Find(compName);
		}

		for (const auto& [fieldName, fieldVal] : compOverride.AsObject())
			(*baseComp)[fieldName] = fieldVal;
	}
}

// Resolve an AssetID stored as a JSON number to an absolute path via the AssetRegistry.
static std::string ResolveAssetIDFromJSON(const JsonValue& val)
{
	if (!val.IsNumber()) return {};
	AssetID id;
	id.Raw = static_cast<int64_t>(val.AsNumber());
	return AssetRegistry::Get().ResolvePath(id);
}

// Forward declaration for mutual recursion.
static size_t SpawnSceneInternal(Registry* reg, const JsonValue& sceneJson, bool bBackground);

// Spawn all entities from a prefab or scene JSON with full recursive prefab support.
// Returns number of entities spawned. Construct prefabs are skipped (count = 0).
static size_t SpawnFromAssetJSON(Registry* reg, const std::string& path,
								 const JsonValue* componentOverrides, bool bBackground)
{
	if (ActivePrefabLoads.count(path))
	{
		LOG_ENG_ERROR_F("[EntityBuilder] Prefab cycle detected at '%s'", path.c_str());
		return 0;
	}

	ActivePrefabLoads.insert(path);

	JsonValue assetJson = LoadAssetJSON(path);
	size_t count        = 0;

	if (!assetJson.IsNull())
	{
		// Skip construct prefabs — they are spawned by the construct factory, not EntityBuilder.
		const JsonValue* prefabType = assetJson.Find("prefabType");
		if (prefabType && prefabType->IsString() && prefabType->AsString() == "Construct")
		{
			// no-op
		}
		else if (assetJson.Find("entities"))
		{
			// Scene / entity-group prefab — recurse.
			count = SpawnSceneInternal(reg, assetJson, bBackground);
		}
		else if (assetJson.Find("type"))
		{
			// Single entity prefab — apply overrides then spawn.
			MergeComponentOverrides(assetJson, componentOverrides);
			EntityHandle id = EntityBuilder::SpawnEntity(reg, assetJson, bBackground);
			if (id.IsValid()) ++count;
		}
		else
		{
			LOG_ENG_WARN_F("[EntityBuilder] Unrecognized asset format: '%s'", path.c_str());
		}
	}

	ActivePrefabLoads.erase(path);
	return count;
}

static size_t SpawnSceneInternal(Registry* reg, const JsonValue& sceneJson, bool bBackground)
{
	const JsonValue* entities = sceneJson.Find("entities");
	if (!entities || !entities->IsArray())
	{
		LOG_ENG_ERROR("[EntityBuilder] Scene/group missing 'entities' array");
		return 0;
	}

	size_t count = 0;
	for (const auto& entry : entities->AsArray())
	{
		// Prefab reference: { "prefab": <assetID>, "overrides": { "CTransform": { ... } } }
		const JsonValue* prefabRef = entry.Find("prefab");
		if (prefabRef && prefabRef->IsNumber())
		{
			std::string path = ResolveAssetIDFromJSON(*prefabRef);
			if (path.empty())
			{
				LOG_ENG_WARN_F("[EntityBuilder] Prefab AssetID %lld not found in registry",
							   static_cast<long long>(static_cast<int64_t>(prefabRef->AsNumber())));
				continue;
			}
			count += SpawnFromAssetJSON(reg, path, entry.Find("overrides"), bBackground);
		}
		else
		{
			// Inline entity definition.
			EntityHandle id = EntityBuilder::SpawnEntity(reg, entry, bBackground);
			if (id.IsValid()) ++count;
		}
	}
	return count;
}

size_t EntityBuilder::SpawnScene(Registry* reg, const JsonValue& sceneJson, bool bBackground)
{
	size_t count = SpawnSceneInternal(reg, sceneJson, bBackground);

	const JsonValue* nameVal = sceneJson.Find("name");
	const char* sceneName    = (nameVal && nameVal->IsString()) ? nameVal->AsString().c_str() : "unnamed";
	LOG_ENG_INFO_F("[EntityBuilder] Spawned %zu entities from scene '%s'", count, sceneName);

	return count;
}

size_t EntityBuilder::SpawnFromFile(Registry* reg, const char* filePath, bool bBackground)
{
	// Use the full recursive path so top-level loads also participate in cycle detection.
	return SpawnFromAssetJSON(reg, filePath, nullptr, bBackground);
}

size_t EntityBuilder::SpawnFromFileTracked(Registry* reg, const char* filePath, bool bBackground,
										   std::vector<GlobalEntityHandle>& outHandles)
{
	ActiveHandleCollector = &outHandles;
	size_t count          = SpawnFromAssetJSON(reg, filePath, nullptr, bBackground);
	ActiveHandleCollector = nullptr;
	return count;
}

EntityHandle EntityBuilder::SpawnEntityFromFile(Registry* reg, const char* filePath, bool bBackground)
{
	JsonValue assetJson = LoadAssetJSON(filePath);
	if (assetJson.IsNull()) return EntityHandle{};

	// Only handle single-entity prefabs — scenes and construct prefabs are not valid here.
	if (!assetJson.Find("type") || assetJson.Find("entities"))
	{
		LOG_ENG_WARN_F("[EntityBuilder] SpawnEntityFromFile: '%s' is not a single-entity prefab", filePath);
		return EntityHandle{};
	}

	return SpawnEntity(reg, assetJson, bBackground);
}

// ---------------------------------------------------------------------------
// Save helpers
// ---------------------------------------------------------------------------

// Read a numeric value from a raw field pointer and return it as a JsonValue.
static JsonValue ReadFieldValue(const void* src, size_t fieldSize, FieldValueType valueType)
{
	switch (valueType)
	{
		case FieldValueType::Fixed32:
			{
				SimFloat f;
				std::memcpy(&f, src, 4);
				return JsonValue::Number(f.ToDouble());
			}
		case FieldValueType::Float32:
			{
				SimFloat f;
				std::memcpy(&f, src, 4);
				return JsonValue::Number(f.ToDouble());
			}
		case FieldValueType::Float64:
			{
				double d;
				std::memcpy(&d, src, 8);
				return JsonValue::Number(d);
			}
		case FieldValueType::Int32:
			{
				int32_t v;
				std::memcpy(&v, src, 4);
				return JsonValue::Number(static_cast<double>(v));
			}
		case FieldValueType::Uint32:
			{
				uint32_t v;
				std::memcpy(&v, src, 4);
				return JsonValue::Number(static_cast<double>(v));
			}
		case FieldValueType::Int64:
			{
				int64_t v;
				std::memcpy(&v, src, 8);
				return JsonValue::Number(static_cast<double>(v));
			}
		case FieldValueType::Uint64:
			{
				uint64_t v;
				std::memcpy(&v, src, 8);
				return JsonValue::Number(static_cast<double>(v));
			}
		default:
			// Fallback: dispatch on size
			switch (fieldSize)
			{
				case 4:
					{
						SimFloat f;
						std::memcpy(&f, src, 4);
						return JsonValue::Number(f.ToDouble());
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
}

// ---------------------------------------------------------------------------
// EntityBuilder — Save
// ---------------------------------------------------------------------------

JsonValue EntityBuilder::SerializeEntity(Registry* reg, Archetype* arch, size_t chunkIdx, uint32_t localIndex)
{
	JsonValue entity = JsonValue::Object();

	// Entity type name
	const auto& MR       = ReflectionRegistry::Get();
	const char* typeName = MR.EntityGetters[arch->ArchClassID].Name;
	entity["type"]       = JsonValue::String(typeName ? typeName : "unknown");

	// Build field array table for reading
	Chunk* chunk = arch->Chunks[chunkIdx];
	void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
	arch->BuildFieldArrayTable(chunk, fieldArrayTable,
							   reg->GetTemporalCache()->GetActiveWriteFrame(),
							   reg->GetVolatileCache()->GetActiveWriteFrame());

	// Group fields by component
	const auto& CFR      = ReflectionRegistry::Get();
	JsonValue components = JsonValue::Object();

	// Track which component we're building — fields are contiguous per component
	ComponentTypeID currentCompID = 0;
	JsonValue* currentComp        = nullptr;

	for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
	{
		size_t idx = fdesc.fieldSlotIndex;
		if (!fieldArrayTable[idx]) continue;

		// Look up field name from ComponentFieldRegistry
		const auto* fields    = CFR.GetFields(fdesc.componentID);
		const char* fieldName = (fields && fdesc.componentSlotIndex < fields->size())
									? (*fields)[fdesc.componentSlotIndex].Name
									: nullptr;
		if (!fieldName) continue;

		// Look up component name when the component ID changes
		if (fdesc.componentID != currentCompID)
		{
			currentCompID        = fdesc.componentID;
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
		const auto* base          = static_cast<const uint8_t*>(fieldArrayTable[idx]);
		(*currentComp)[fieldName] = ReadFieldValue(base + localIndex * fdesc.fieldSize, fdesc.fieldSize, fdesc.valueType);
	}

	entity["components"] = std::move(components);
	return entity;
}

EntityBuilder::SceneMeta EntityBuilder::ParseSceneMeta(const JsonValue& sceneJson)
{
	SceneMeta meta;
	if (const auto* v = sceneJson.Find("name"); v && v->IsString()) meta.Name = v->AsString();
	if (const auto* v = sceneJson.Find("defaultState"); v && v->IsString()) meta.DefaultState = v->AsString();
	if (const auto* v = sceneJson.Find("defaultMode"); v && v->IsString()) meta.DefaultMode = v->AsString();
	return meta;
}

JsonValue EntityBuilder::SerializeScene(Registry* reg, const char* sceneName,
										const char* defaultState, const char* defaultMode)
{
	JsonValue scene = JsonValue::Object();
	scene["name"]   = JsonValue::String(sceneName);
	if (defaultState && defaultState[0]) scene["defaultState"] = JsonValue::String(defaultState);
	if (defaultMode && defaultMode[0]) scene["defaultMode"] = JsonValue::String(defaultMode);

	JsonValue entities = JsonValue::Array();

	// Build field array table to check Active flags
	const auto& archetypes = reg->GetArchetypes();

	for (const auto& [key, arch] : archetypes)
	{
		for (size_t ci = 0; ci < arch->Chunks.size(); ++ci)
		{
			uint32_t count = arch->GetAllocatedChunkCount(ci);

			// Build field table to read flags
			void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
			arch->BuildFieldArrayTable(arch->Chunks[ci], fieldArrayTable,
									   reg->GetTemporalCache()->GetActiveWriteFrame(),
									   reg->GetVolatileCache()->GetActiveWriteFrame());

			// Flags are at field index 0 by convention (CacheSlotMeta::Flags)
			const auto* flagsArr = (arch->GetFieldArrayCount() > 0 && fieldArrayTable[0])
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

	LOG_ENG_INFO_F("[EntityBuilder] Serialized %zu entities into scene '%s'", entityCount, sceneName);
	return scene;
}

bool EntityBuilder::SaveToFile(Registry* reg, const char* sceneName, const char* filePath,
							   const char* defaultState, const char* defaultMode)
{
	JsonValue scene  = SerializeScene(reg, sceneName, defaultState, defaultMode);
	std::string json = JsonWrite(scene, true);

	std::ofstream file(filePath);
	if (!file.is_open())
	{
		LOG_ENG_ERROR_F("[EntityBuilder] Failed to open file '%s' for writing", filePath);
		return false;
	}

	file << json;
	file.close();

	LOG_ENG_INFO_F("[EntityBuilder] Saved scene to '%s'", filePath);
	return true;
}