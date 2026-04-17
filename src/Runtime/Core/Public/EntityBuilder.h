#pragma once

#include "AssetRegistry.h"
#include "Json.h"
#include "Logger.h"
#include "Types.h"
#include <string>
#include <vector>

#include "EntityRecord.h"
#include "RegistryTypes.h"

class Registry;

// Data-driven entity spawning from JSON.
//
// Reads entity type names and component field values from JSON, resolves them
// to integer IDs via MetaRegistry/ComponentFieldRegistry, and hydrates field
// arrays through the existing reflection system. String comparison happens
// only here at the load boundary — runtime uses integer IDs exclusively.
//
// Must be called within a SpawnSync callback or before threads start.

class Archetype;

struct EntityBuilder
{
	// --- Load ---

	// Spawn a single entity from a JSON object.
	// Expected format:
	//   { "type": "CubeEntity", "components": { "TransRot": { "PosX": 1.0, ... }, ... } }
	// bBackground: entity is Alive but not Active — won't tick or render until an
	// explicit Alive→Active sweep (used for background/client level loads).
	// Returns the created EntityID, or an invalid ID on failure.
	static EntityHandle SpawnEntity(Registry* reg, const JsonValue& entityJson, bool bBackground = false);

	// Spawn all entities described in a scene JSON.
	// Expected format:
	//   { "name": "SceneName", "entities": [ { "type": "...", "components": { ... } }, ... ] }
	// bBackground: see SpawnEntity.
	// Returns the number of entities successfully spawned.
	static size_t SpawnScene(Registry* reg, const JsonValue& sceneJson, bool bBackground = false);

	// Load a .prefab or .tnxscene file from disk and spawn its contents.
	// Returns the number of entities spawned (1 for prefab, N for scene).
	// bBackground: see SpawnEntity. File I/O and JSON parse are synchronous on the
	// calling thread — true async parsing is a future improvement.
	static size_t SpawnFromFile(Registry* reg, const char* filePath, bool bBackground = false);

	// Load by AssetID — resolves path via AssetRegistry::ResolvePath.
	static size_t SpawnFromAsset(Registry* reg, const AssetID& id, bool bBackground = false)
	{
		std::string path = AssetRegistry::Get().ResolvePath(id);
		if (path.empty()) return 0;
		return SpawnFromFile(reg, path.c_str(), bBackground);
	}

	// Typed prefab spawn — loads a single-entity prefab from an AssetID and creates
	// an entity of type TEntity. Asset-ref fields stored as strings are wired through
	// the checkout system. Use from ConstructView::Initialize(owner, assetID).
	template <template <FieldWidth> class TEntity>
	static EntityHandle SpawnTyped(Registry* reg, AssetID prefabID, bool bBackground = false)
	{
		std::string path = AssetRegistry::Get().ResolvePath(prefabID);
		if (path.empty())
		{
			LOG_ENG_ERROR_F("[EntityBuilder] SpawnTyped: AssetID not found in registry (raw=%lld)",
							static_cast<long long>(prefabID.Raw));
			return EntityHandle{};
		}
		return SpawnEntityFromFile(reg, path.c_str(), bBackground);
	}

	// Load a single-entity prefab from a file path and spawn it.
	// Returns an invalid handle if the file is a scene or construct prefab.
	static EntityHandle SpawnEntityFromFile(Registry* reg, const char* filePath, bool bBackground = false);

	// --- Scene metadata ---

	struct SceneMeta
	{
		std::string Name;
		std::string DefaultState; // FlowState to load (empty = none)
		std::string DefaultMode;  // GameMode to activate (empty = none)
	};

	// Parse scene-level metadata from a JSON scene without spawning entities.
	static SceneMeta ParseSceneMeta(const JsonValue& sceneJson);

	// --- Save ---

	// Serialize a single entity at the given local index within a chunk.
	// Returns a JSON object: { "type": "...", "components": { ... } }
	static JsonValue SerializeEntity(Registry* reg, Archetype* arch, size_t chunkIdx, uint32_t localIndex);

	// Serialize all entities in the registry into a scene JSON.
	// Returns: { "name": "...", "entities": [ ... ], "defaultState": "...", "defaultMode": "..." }
	static JsonValue SerializeScene(Registry* reg, const char* sceneName,
									const char* defaultState = nullptr, const char* defaultMode = nullptr);

	// Save a scene to disk. Returns true on success.
	static bool SaveToFile(Registry* reg, const char* sceneName, const char* filePath,
						   const char* defaultState = nullptr, const char* defaultMode = nullptr);
};
