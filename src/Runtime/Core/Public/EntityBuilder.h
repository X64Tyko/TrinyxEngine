#pragma once

#include "Json.h"
#include "Types.h"
#include <vector>

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
	// Returns the created EntityID, or an invalid ID on failure.
	static EntityHandle SpawnEntity(Registry* reg, const JsonValue& entityJson);

	// Spawn all entities described in a scene JSON.
	// Expected format:
	//   { "name": "SceneName", "entities": [ { "type": "...", "components": { ... } }, ... ] }
	// Returns the number of entities successfully spawned.
	static size_t SpawnScene(Registry* reg, const JsonValue& sceneJson);

	// Load a .prefab or .tnxscene file from disk and spawn its contents.
	// Returns the number of entities spawned (1 for prefab, N for scene).
	static size_t SpawnFromFile(Registry* reg, const char* filePath);

	// --- Save ---

	// Serialize a single entity at the given local index within a chunk.
	// Returns a JSON object: { "type": "...", "components": { ... } }
	static JsonValue SerializeEntity(Registry* reg, Archetype* arch, size_t chunkIdx, uint32_t localIndex);

	// Serialize all entities in the registry into a scene JSON.
	// Returns: { "name": "...", "entities": [ ... ] }
	static JsonValue SerializeScene(Registry* reg, const char* sceneName);

	// Save a scene to disk. Returns true on success.
	static bool SaveToFile(Registry* reg, const char* sceneName, const char* filePath);
};
