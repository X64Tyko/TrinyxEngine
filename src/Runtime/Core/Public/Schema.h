#pragma once
#include <functional>
#include <Logger.h>
#include <string_view>
#include <tuple>

#include "EntityInvoke.h"
#include "FieldMeta.h"
#include "Profiler.h"
#include "Signature.h"
#include "Types.h"

#define REGISTER_ENTITY_PREPHYS(Type, ClassID) \
    case ClassID: InvokePrePhysicsImpl<Type>(dt, fieldArrayTable, componentCount); break;

class MetaRegistry
{
public:
	static MetaRegistry& Get()
	{
		static MetaRegistry instance; // Thread-safe magic static
		return instance;
	}

	std::unordered_map<ClassID, ComponentSignature> ClassToArchetype;
	std::unordered_map<ClassID, std::vector<ComponentTypeID>> ClassToComponentList;
	std::unordered_map<Signature, std::vector<ClassID>> ArchetypeToClass;
	std::unordered_map<ClassID, SystemID> ClassSystemID;
	EntityMeta EntityGetters[4096];

	// Reverse lookup: entity name → ClassID. Returns 0 if not found.
	[[nodiscard]] ClassID GetEntityByName(std::string_view name) const
	{
		auto it = NameToClassID.find(name);
		return it != NameToClassID.end() ? it->second : 0;
	}

	template <typename T>
	void RegisterPrefab()
	{
		const ClassID ID           = T::StaticClassID();
		EntityGetters[ID].ViewSize = sizeof(T);

		if constexpr (requires { T::EntityTypeName; })
		{
			EntityGetters[ID].Name                             = T::EntityTypeName;
			NameToClassID[std::string_view(T::EntityTypeName)] = ID;
		}

		if constexpr (HasScalarUpdate<T>)
		{
			EntityGetters[ID].ScalarUpdate = InvokeScalarUpdateImpl<T>;
		}

		if constexpr (HasPrePhysics<T>)
		{
			EntityGetters[ID].PrePhys = InvokePrePhysicsImpl<T>;
		}

		if constexpr (HasPostPhysics<T>)
		{
			EntityGetters[ID].PostPhys = InvokePostPhysicsImpl<T>;
		}

		if constexpr (requires { T::EntitiesPerChunk; })
		{
			EntityGetters[ID].EntitiesPerChunk = T::EntitiesPerChunk;
		}
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
		if constexpr (requires { T::SystemTypeID; }) ClassSystemID[ID] = ClassSystemID[ID] | T::SystemTypeID;

		if constexpr (requires { T::StaticTemporalIndex(); })
		{
			//ComponentFieldRegistry::Get().SetCacheSlotIndex(TypeID, T::StaticTemporalIndex());
		}
	}

private:
	std::unordered_map<std::string_view, ClassID> NameToClassID;
};

// The container for member pointers
template <typename... Members>
struct SchemaDefinition
{
	std::tuple<Members...> members;

	constexpr SchemaDefinition(Members... m)
		: members(m...)
	{
	}

	// EXTEND: Allows derived classes to append their own members
	template <typename... NewMembers>
	constexpr auto Extend(NewMembers... newMembers) const
	{
		return std::apply([&](auto... currentMembers)
		{
			return SchemaDefinition<Members..., NewMembers...>(currentMembers..., newMembers...);
		}, members);
	}

	template <typename Target, typename Replacement>
	constexpr auto Replace(Target target, Replacement replacement) const
	{
		// Unpack the existing tuple...
		return std::apply([&](auto... args)
		{
			// ...and rebuild a NEW SchemaDefinition
			return SchemaDefinition<decltype(ResolveReplacement(args, target, replacement))...>(
				ResolveReplacement(args, target, replacement)...
			);
		}, members);
	}

private:
	// Helper: Selects either the 'current' item or the 'replacement'
	// based on whether 'current' matches the 'target' we want to remove.
	template <typename Current, typename Target, typename Replacement>
	static constexpr auto ResolveReplacement(Current current, Target target, Replacement replacement)
	{
		// 1. Check if types match first (Optimization + Safety)
		if constexpr (std::is_same_v<Current, Target>)
		{
			// 2. Check value - use ternary to ensure consistent return type
			return (current == target) ? replacement : current;
		}
		else
		{
			// Not the droid we are looking for. Keep existing.
			return current;
		}
	}
};

// The static builder interface
struct Schema
{
	template <typename... Args>
	static constexpr auto Create(Args... args)
	{
		return SchemaDefinition<Args...>(args...);
	}
};
