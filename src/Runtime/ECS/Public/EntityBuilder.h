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

/// @brief Data-driven entity spawning from JSON scene and prefab files.
///
/// Reads entity type names and component field values from JSON, resolves them
/// to integer IDs via @ref ReflectionRegistry, and hydrates field arrays through
/// the existing reflection system. String comparisons happen only at the load
/// boundary — all runtime paths use integer IDs.
///
/// All spawn methods must be called within a @c SpawnSync callback or before
/// engine threads start.

class Archetype;

struct EntityBuilder
{
	/// @brief Spawn a single entity from a JSON object.
	///
	/// Expected format: @code { "type": "CubeEntity", "components": { "CTransform": { "PosX": 1.0, ... } } } @endcode
	///
	/// @param bBackground If true, entity is @c Alive but not @c Active — will not
	///        tick or render until an explicit Alive→Active sweep.
	/// @return The created handle, or an invalid handle on failure.
	static EntityHandle SpawnEntity(Registry* reg, const JsonValue& entityJson, bool bBackground = false);

	/// @brief Spawn all entities described in a scene JSON.
	///
	/// Expected format: @code { "name": "SceneName", "entities": [ ... ] } @endcode
	///
	/// @param bBackground Passed through to each @ref SpawnEntity call.
	/// @return Number of entities successfully spawned.
	static size_t SpawnScene(Registry* reg, const JsonValue& sceneJson, bool bBackground = false);

	/// @brief Load a @c .prefab or @c .tnxscene file from disk and spawn its contents.
	///
	/// File I/O and JSON parse are synchronous on the calling thread.
	///
	/// @param bBackground See @ref SpawnEntity.
	/// @return Number of entities spawned (1 for prefab, N for scene).
	static size_t SpawnFromFile(Registry* reg, const char* filePath, bool bBackground = false);

	/// @brief Background-load a content chunk by @ref AssetID and register spawned handles.
	///
	/// @c InstanceIndex disambiguates multiple simultaneous loads of the same asset.
	/// Must be called from a logic-thread-synchronized context (@c SpawnAndWait / @c PostAndWait).
	///
	/// @return Number of entities spawned.
	static size_t StreamChunkTracked(Registry* reg, const AssetID& assetID, uint16_t instanceIndex = 0);

	/// @brief Activate entities registered by @ref StreamChunkTracked for @c (assetID, instanceIndex).
	///
	/// Sets @c Active|Dirty|DirtiedFrame on each slot. Returns slab indices for rollback event replay.
	/// Must be called from a logic-thread-synchronized context.
	static std::vector<uint32_t> ActivateStreamedChunk(Registry* reg, int64_t assetIDRaw, uint16_t instanceIndex = 0);

	/// @brief Like @ref SpawnFromFile but also collects @ref GlobalEntityHandle of each spawned entity.
	/// @param[out] outHandles Receives a handle per successfully spawned entity.
	static size_t SpawnFromFileTracked(Registry* reg, const char* filePath, bool bBackground,
		std::vector<GlobalEntityHandle>& outHandles);

	/// @brief Load by @ref AssetID — resolves the path via @c AssetRegistry::ResolvePath.
	static size_t SpawnFromAsset(Registry* reg, const AssetID& id, bool bBackground = false)
	{
		std::string path = AssetRegistry::Get().ResolvePath(id);
		if (path.empty()) return 0;
		return SpawnFromFile(reg, path.c_str(), bBackground);
	}

	/// @brief Typed prefab spawn from an @ref AssetID.
	///
	/// Loads a single-entity prefab and creates an entity of type @c TEntity.
	/// Asset-ref fields are resolved through the checkout system.
	/// Typically called from @c ConstructView::Initialize(owner, assetID).
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

	/// @brief Load a single-entity prefab from a file path and spawn it.
	/// @return Invalid handle if the file is a scene or construct prefab.
	static EntityHandle SpawnEntityFromFile(Registry* reg, const char* filePath, bool bBackground = false);

	/// @brief Scene-level metadata parsed without spawning any entities.
	struct SceneMeta
	{
		std::string Name;
		std::string DefaultState; ///< FlowState name to activate (empty = none).
		std::string DefaultMode;  ///< GameMode name to activate (empty = none).
	};

	/// @brief Parse scene-level metadata from a JSON scene without spawning entities.
	static SceneMeta ParseSceneMeta(const JsonValue& sceneJson);

	/// @brief Serialize a single entity at a given slot within an archetype chunk.
	/// @return JSON object: @code { "type": "...", "components": { ... } } @endcode
	static JsonValue SerializeEntity(Registry* reg, Archetype* arch, size_t chunkIdx, uint32_t localIndex);

	/// @brief Serialize all entities in the registry into a scene JSON document.
	/// @return JSON: @code { "name": "...", "entities": [ ... ], "defaultState": "...", "defaultMode": "..." } @endcode
	static JsonValue SerializeScene(Registry* reg, const char* sceneName,
		const char* defaultState = nullptr, const char* defaultMode = nullptr);

	/// @brief Save a scene to disk.
	/// @return @c true on success.
	static bool SaveToFile(Registry* reg, const char* sceneName, const char* filePath,
		const char* defaultState = nullptr, const char* defaultMode = nullptr);
};
