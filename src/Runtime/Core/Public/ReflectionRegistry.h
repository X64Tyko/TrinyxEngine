#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "EntityInvoke.h"
#include "FieldMeta.h"
#include "Signature.h"
#include "Types.h"

class GameState;
class GameMode;

// ---------------------------------------------------------------------------
// ReflectionRegistry — Single source of truth for all type metadata.
//
// Merges the former MetaRegistry (entity types, signatures, lifecycle
// function pointers) and ComponentFieldRegistry (field decomposition,
// component metadata, tier/slot queries) into one singleton.
//
// Also holds GameState/GameMode factory registrations so FlowManager
// and the editor can discover them without manual registration calls.
//
// Populated at static init time by registration macros (TNX_REGISTER_ENTITY,
// TNX_REGISTER_COMPONENT, TNX_REGISTER_STATE, TNX_REGISTER_MODE).
//
// Supports baking to a file for deterministic IDs across builds. When a
// baked file is loaded, its IDs are authoritative. Function pointers and
// factories are merged from reflected data by name.
//
// Thread safety: all registration happens during static init (before main).
// Read-only after engine init.
// ---------------------------------------------------------------------------
class ReflectionRegistry
{
public:
	static ReflectionRegistry& Get()
	{
		static ReflectionRegistry instance;
		return instance;
	}

	// ===== Entity metadata (from MetaRegistry) =====

	std::unordered_map<ClassID, ComponentSignature>           ClassToArchetype;
	std::unordered_map<ClassID, std::vector<ComponentTypeID>> ClassToComponentList;
	std::unordered_map<Signature, std::vector<ClassID>>       ArchetypeToClass;
	std::unordered_map<ClassID, SystemID>                     ClassSystemID;
	EntityMeta EntityGetters[4096]; // NOT serializable (function pointers)

	// ===== Component metadata (from ComponentFieldRegistry) =====

	std::unordered_map<ComponentTypeID, ComponentMetaEx> ComponentData;

	// ===== Name reverse lookups (std::string — safe for baked data) =====

	std::unordered_map<std::string, ClassID>         NameToClassID;
	std::unordered_map<std::string, ComponentTypeID> NameToComponentID;

	// ===== GameState / GameMode factories =====

	using StateFactory = std::function<std::unique_ptr<GameState>()>;
	using ModeFactory  = std::function<std::unique_ptr<GameMode>()>;

	struct StateEntry { const char* Name; StateFactory Factory; };
	struct ModeEntry  { const char* Name; ModeFactory Factory; };

	std::vector<StateEntry> RegisteredStates;
	std::vector<ModeEntry>  RegisteredModes;

	// ===== Entity registration (called at static init) =====

	template <typename T>
	void RegisterPrefab()
	{
		const ClassID ID           = T::StaticClassID();
		EntityGetters[ID].ViewSize = sizeof(T);

		if constexpr (requires { T::EntityTypeName; })
		{
			EntityGetters[ID].Name             = T::EntityTypeName;
			NameToClassID[T::EntityTypeName]   = ID;
		}

		if constexpr (HasScalarUpdate<T>)
			EntityGetters[ID].ScalarUpdate = InvokeScalarUpdateImpl<T>;

		if constexpr (HasPrePhysics<T>)
			EntityGetters[ID].PrePhys = InvokePrePhysicsImpl<T>;

		if constexpr (HasPostPhysics<T>)
			EntityGetters[ID].PostPhys = InvokePostPhysicsImpl<T>;

		if constexpr (requires { T::EntitiesPerChunk; })
			EntityGetters[ID].EntitiesPerChunk = T::EntitiesPerChunk;
	}

	template <typename C, typename T>
	void RegisterPrefabComponent()
	{
		const ClassID ID             = C::StaticClassID();
		const ComponentTypeID TypeID = T::StaticTypeID();
		ComponentSignature& Def      = ClassToArchetype[ID];
		Def.set(TypeID - 1);

		for (auto& component : ClassToComponentList[ID])
		{
			if (component == TypeID) return;
		}

		ClassToComponentList[ID].push_back(TypeID);
		if constexpr (requires { T::SystemTypeID; })
			ClassSystemID[ID] = ClassSystemID[ID] | T::SystemTypeID;
	}

	// ===== Component field registration (called at static init) =====

	void RegisterFields(ComponentTypeID typeID, const char* name,
						std::vector<FieldMeta>&& fields, CacheTier tier, uint8_t slot);

	// ===== Entity accessors =====

	[[nodiscard]] ClassID GetEntityByName(std::string_view name) const;

	// ===== Component accessors =====

	[[nodiscard]] const std::vector<FieldMeta>* GetFields(ComponentTypeID typeID) const;
	[[nodiscard]] bool IsDecomposed(ComponentTypeID typeID) const;
	[[nodiscard]] size_t GetFieldCount(ComponentTypeID typeID) const;
	[[nodiscard]] CacheTier GetTemporalTier(ComponentTypeID typeID) const;
	[[nodiscard]] uint8_t GetCacheSlotIndex(ComponentTypeID typeID) const;
	[[nodiscard]] const ComponentMetaEx& GetComponentMeta(ComponentTypeID typeID) const;
	[[nodiscard]] ComponentTypeID GetComponentByName(std::string_view name) const;
	[[nodiscard]] const std::unordered_map<ComponentTypeID, ComponentMetaEx>& GetAllComponents() const
	{
		return ComponentData;
	}

	void SetCacheSlotIndex(ComponentTypeID typeID, uint8_t slot);

	// ===== GameState / GameMode registration =====

	void RegisterState(const char* name, StateFactory factory);
	void RegisterMode(const char* name, ModeFactory factory);

	[[nodiscard]] StateFactory FindStateFactory(const char* name) const;
	[[nodiscard]] ModeFactory FindModeFactory(const char* name) const;

	// ===== Bake / Snapshot / Diff (Step 5-6, stubs for now) =====

	// TODO: TakeSnapshot, BakeToFile, LoadFromSnapshot, MergeNonSerializable, Diff

private:
	ReflectionRegistry() = default;
};
