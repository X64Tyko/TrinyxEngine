#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include "ScriptComponent.h"

/// Per-script-component flat storage — one instance per registered schema.
/// Indexed by EntityCacheHandle (the entity's SoA slab slot).
/// AoS layout: BlobSize bytes per entity, contiguous, sized at Initialize time.
struct ScriptComponentStorage
{
	ScriptSchema Schema;
	uint32_t Capacity = 0;

	std::unique_ptr<uint8_t[]> Blobs;   ///< Schema.BlobSize * Capacity bytes.
	std::unique_ptr<uint8_t[]> HasComp; ///< 1 byte per entity: 1 = attached, 0 = not.

	void Initialize(uint32_t capacity);

	/// Re-allocate Blobs with newBlobSize, copying existing data per-entity.
	/// Called by ScriptRegistry::AddField when blobs are already live.
	void GrowBlobs(uint16_t oldBlobSize, uint16_t newBlobSize);
};

/// Singleton registry for runtime-defined component schemas and their blob storage.
///
/// Intended for early-project iteration: define components in the editor without
/// recompiling. Storage is AoS (no SIMD sweep, no rollback). When a schema is stable,
/// use ComponentGeneratorPanel to export a compile-time .h and migrate.
///
/// Thread safety: schema mutations (Register*, AddField) must occur outside the Logic
/// thread hot path — during handshake or while PIE is stopped.
/// Field reads/writes (Get*/Set*) are expected from the Logic thread only.
class ScriptRegistry
{
public:
	static ScriptRegistry& Get();

	/// Called once at World init. Allocates blob storage for all already-registered schemas.
	/// @param maxCachedEntities  MAX_CACHED_ENTITIES from EngineConfig (post-ApplyDefaults).
	void Initialize(uint32_t maxCachedEntities);

	/// Release all blob storage. Called on World shutdown.
	void Shutdown();

	// ── Schema management ──────────────────────────────────────────────────

	/// Register a new script component. Safe to call before Initialize.
	/// Returns the assigned ComponentTypeID, or 0 on failure.
	ComponentTypeID RegisterComponent(const char* name, SystemID sysID, CacheTier tier);

	/// Add a field to an existing schema. Zero-initialises the field in all live blobs.
	/// Returns false if the TypeID is unknown, the field limit is reached, or the name collides.
	bool AddField(ComponentTypeID id, const char* fieldName, ScriptFieldType type);

	/// Register from a complete field list. Adds only new trailing fields (additive).
	/// Sets *bBreakingChange = true if existing fields were removed or reordered.
	/// Returns the TypeID (existing or new), or 0 on hard failure.
	ComponentTypeID RegisterOrUpdate(const char* name, SystemID sysID, CacheTier tier,
		const ScriptFieldType* fieldTypes,
		const char* const* fieldNames,
		uint8_t fieldCount,
		bool* bBreakingChange = nullptr);

	const ScriptSchema* GetSchema(ComponentTypeID id) const;
	const ScriptSchema* GetSchema(const char* name) const;
	const ScriptComponentStorage* GetStorage(ComponentTypeID id) const;

	void ForEachSchema(const std::function<void(const ScriptSchema&,
			const ScriptComponentStorage&)>& fn) const;

	uint8_t GetComponentCount() const { return ComponentCount; }

	// ── Entity attachment ──────────────────────────────────────────────────

	void Attach(uint32_t cacheEntityIndex, ComponentTypeID id);
	void Detach(uint32_t cacheEntityIndex, ComponentTypeID id);
	bool HasComponent(uint32_t cacheEntityIndex, ComponentTypeID id) const;

	/// Called by DefragSystem when an entity moves from oldIndex to newIndex.
	void OnEntityMoved(uint32_t oldIndex, uint32_t newIndex);

	// ── Typed field access (by byte offset into the blob) ─────────────────

	float GetFloat(uint32_t cacheIdx, ComponentTypeID id, uint16_t offset) const;
	void SetFloat(uint32_t cacheIdx, ComponentTypeID id, uint16_t offset, float val);
	int32_t GetInt32(uint32_t cacheIdx, ComponentTypeID id, uint16_t offset) const;
	void SetInt32(uint32_t cacheIdx, ComponentTypeID id, uint16_t offset, int32_t val);
	uint32_t GetUInt32(uint32_t cacheIdx, ComponentTypeID id, uint16_t offset) const;
	void SetUInt32(uint32_t cacheIdx, ComponentTypeID id, uint16_t offset, uint32_t val);
	bool GetBool(uint32_t cacheIdx, ComponentTypeID id, uint16_t offset) const;
	void SetBool(uint32_t cacheIdx, ComponentTypeID id, uint16_t offset, bool val);

	/// Named field lookup — linear search, editor use only.
	const ScriptFieldDesc* FindField(ComponentTypeID id, const char* fieldName) const;

private:
	ScriptComponentStorage* FindStorage(ComponentTypeID id);
	const ScriptComponentStorage* FindStorage(ComponentTypeID id) const;

	uint32_t MaxEntities   = 0;
	uint8_t ComponentCount = 0;

	// Fixed array — stable addresses keep FieldMeta::Name pointers valid in ReflectionRegistry.
	ScriptComponentStorage Components[kMaxScriptComponents];

	std::unordered_map<std::string, uint8_t> NameToIndex; // component name → Components[] index
	std::unordered_map<uint32_t, uint8_t> IDToIndex;      // TypeID         → Components[] index
};
