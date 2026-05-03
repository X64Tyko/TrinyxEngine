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

class FlowState;
class GameMode;
class ConstructRegistry;
class WorldBase;
class Soul;
union EntityHandle;

// Forward declarations for RPC dispatch table.
// Full definitions live in RPC.h / NetTypes.h — included by ReflectionRegistry.cpp.
class  Soul;
struct RPCContext;
struct RPCHeader;
using  SoulRPCHandler = void(*)(Soul*, const RPCContext&, const uint8_t*);

// ---------------------------------------------------------------------------
// ReflectionRegistry — Single source of truth for all type metadata.
//
// Merges the former MetaRegistry (entity types, signatures, lifecycle
// function pointers) and ComponentFieldRegistry (field decomposition,
// component metadata, tier/slot queries) into one singleton.
//
// Also holds FlowState/GameMode factory registrations so FlowManager
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

	// ===== FlowState / GameMode factories =====

	using StateFactory = std::function<std::unique_ptr<FlowState>()>;
	using ModeFactory  = std::function<std::unique_ptr<GameMode>()>;

	struct StateEntry
	{
		const char* Name;
		int64_t UUID;
		StateFactory Factory;
	};

	struct ModeEntry
	{
		const char* Name;
		int64_t UUID;
		ModeFactory Factory;
	};

	std::vector<StateEntry> RegisteredStates;
	std::vector<ModeEntry>  RegisteredModes;

	std::unordered_map<int64_t, size_t> StateUUIDIndex; // UUID → RegisteredStates index
	std::unordered_map<int64_t, size_t> ModeUUIDIndex;  // UUID → RegisteredModes index

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

		EntityGetters[ID].Initialize = InvokeInitializeImpl<T>;

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

	// ===== FlowState / GameMode registration =====

	void RegisterState(const char* name, StateFactory factory);
	void RegisterMode(const char* name, ModeFactory factory);

	[[nodiscard]] StateFactory FindStateFactory(const char* name) const;
	[[nodiscard]] ModeFactory FindModeFactory(const char* name) const;
	[[nodiscard]] StateFactory FindStateByUUID(int64_t uuid) const;
	[[nodiscard]] ModeFactory FindModeByUUID(int64_t uuid) const;

	// ===== ModeMixin registration =====
	//
	// Engine-defined mixins (WithSpawnManagement, WithTeamAssignment, etc.) declare
	// a compile-time BaseTypeID constant and self-register via RegisterMixin.
	// User-defined mixins use TNX_REGISTER_MODEMIX which assigns the next ID from
	// the 128–255 user band via a static counter. IDs are asserted unique at startup.
	//
	// BaseTypeID is the first message type ID claimed by the mixin. Each mixin
	// claims a contiguous band of 4 IDs (BaseTypeID .. BaseTypeID+3).

	static constexpr uint8_t MixinUserBandStart = 128;
	static constexpr uint8_t MixinUserBandEnd   = 255;

	struct MixinEntry
	{
		const char* Name;
		int64_t UUID;
		uint8_t BaseTypeID; // First of 4 contiguous message type IDs
		bool IsUserDefined;
	};

	std::vector<MixinEntry> RegisteredMixins;
	std::unordered_map<uint8_t, size_t> MixinIDIndex; // BaseTypeID → RegisteredMixins index

	void RegisterMixin(const char* name, uint8_t baseTypeID, bool isUserDefined);

	[[nodiscard]] const MixinEntry* FindMixin(const char* name) const;
	[[nodiscard]] const MixinEntry* FindMixinByID(uint8_t baseTypeID) const;

	// ===== SoulRPC dispatch table =====
	//
	// RegisterServerRPC / RegisterClientRPC are called at static init time by the
	// TNX_IMPL_SERVER / TNX_IMPL_CLIENT registrar lambdas in each Soul .cpp file.
	// DispatchServerRPC / DispatchClientRPC are called by FlowManager when a
	// NetMessageType::SoulRPC packet arrives on the server or client respectively.
	//
	// Tables are flat vectors indexed by MethodID for O(1) dispatch.
	// Read-only after engine init (all registration happens at static init).

	struct RPCEntry
	{
		uint16_t       ParamSize = 0;
		SoulRPCHandler Handler   = nullptr;
	};

	void RegisterServerRPC(uint16_t methodID, uint16_t paramSize, SoulRPCHandler handler);
	void RegisterClientRPC(uint16_t methodID, uint16_t paramSize, SoulRPCHandler handler);

	// Returns false if MethodID is out of range, ParamSize mismatches, or no handler registered.
	bool DispatchServerRPC(Soul* soul, const RPCContext& ctx, const RPCHeader& hdr, const uint8_t* params) const;
	bool DispatchClientRPC(Soul* soul, const RPCContext& ctx, const RPCHeader& hdr, const uint8_t* params) const;

	/// Compute a deterministic UUID from a type + name (FNV-1a hash).
	static int64_t UUIDFromName(const char* name);

	/// Compute a 16-bit type hash from a Construct type name.
	/// Used as ConstructNetManifest::PrefabIndex to identify the type on the client.
	static uint16_t ConstructTypeHashFromName(const char* name);

	/// Publish code-registered types (states, modes, entities) into AssetRegistry.
	void PublishToAssetRegistry() const;

	// ===== Replicated Construct factory table =====
	//
	// TNX_REGISTER_CONSTRUCT(T) populates this table at static init time.
	// HandleConstructSpawn calls FindConstructClientFactory(typeHash) to get the
	// client-side factory for the type identified by ConstructNetManifest::PrefabIndex.

	using ConstructClientFactory = void*(*)(ConstructRegistry*, WorldBase*, EntityHandle*, uint8_t viewCount, Soul* ownerSoul);

	struct ConstructEntry
	{
		const char* Name;
		uint16_t TypeHash;
		ConstructClientFactory ClientFactory;
	};

	void RegisterConstruct(const char* name, uint16_t typeHash, ConstructClientFactory factory);
	[[nodiscard]] ConstructClientFactory FindConstructClientFactory(uint16_t typeHash) const;

	std::vector<ConstructEntry> RegisteredConstructs;

	// ===== Bake / Snapshot / Diff (Step 5-6, stubs for now) =====

	// TODO: TakeSnapshot, BakeToFile, LoadFromSnapshot, MergeNonSerializable, Diff

private:
	ReflectionRegistry() = default;

	// RPC dispatch tables — flat vectors indexed by MethodID for O(1) lookup.
	std::vector<RPCEntry> ServerRPCTable;
	std::vector<RPCEntry> ClientRPCTable;
};
