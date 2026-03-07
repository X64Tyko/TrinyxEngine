#pragma once
#include <functional>
#include <Logger.h>
#include <tuple>

#include "FieldMeta.h"
#include "Profiler.h"
#include "Signature.h"
#include "Types.h"


enum class SystemID : uint8_t;
// Helper concept to detect if T has a specific method
template <typename T> concept HasOnCreate = requires(T t) { t.OnCreate(); };
template <typename T> concept HasOnDestroy = requires(T t) { t.OnDestroy(); };
template <typename T> concept HasScalarUpdate = requires(T t, double dt) { t.ScalarUpdate(dt); };
template <typename T> concept HasPrePhysics = requires(T t, double dt) { t.PrePhysics(dt); };
template <typename T> concept HasPostPhysics = requires(T t, double dt) { t.PostPhysics(dt); };
template <typename T> concept HasOnActivate = requires(T t) { t.OnActivate(); };
template <typename T> concept HasOnDeactivate = requires(T t) { t.OnDeactivate(); };
template <typename T> concept HasOnCollide = requires(T t) { t.OnCollide(); };
template <typename T> concept HasDefineSchema = requires(T t) { t.DefineSchema(); };
template <typename T> concept HasDefineFields = requires(T t) { t.DefineFields(); };

using UpdateFunc = void(*)(double, void**, uint8_t*, uint32_t);

#define REGISTER_ENTITY_PREPHYS(Type, ClassID) \
    case ClassID: InvokePrePhysicsImpl<Type>(dt, fieldArrayTable, componentCount); break;

struct EntityMeta
{
	size_t ViewSize = 0;

	UpdateFunc PrePhys      = nullptr;
	UpdateFunc PostPhys     = nullptr;
	UpdateFunc ScalarUpdate = nullptr;

	uint32_t EntitiesPerChunk = 0; // 0 = auto-compute from chunk size

	EntityMeta()
	{
	}

	EntityMeta(const size_t inViewSize, const UpdateFunc prePhys, const UpdateFunc postPhys, const UpdateFunc scalarUpdate)
		: ViewSize(inViewSize)
		, PrePhys(prePhys)
		, PostPhys(postPhys)
		, ScalarUpdate(scalarUpdate)
	{
	}

	EntityMeta(const EntityMeta& rhs)
		: ViewSize(rhs.ViewSize)
		, PrePhys(rhs.PrePhys)
		, PostPhys(rhs.PostPhys)
		, ScalarUpdate(rhs.ScalarUpdate)
		, EntitiesPerChunk(rhs.EntitiesPerChunk)
	{
	}
};

template <typename T>
FORCE_INLINE void InvokePrePhysicsImpl(double dt, void** fieldArrayTable, uint8_t* FlagBase, uint32_t componentCount)
{
	alignas(32) typename T::WideType viewBatch;

	constexpr uint32_t SIMD_BATCH = 8;
	const uint32_t batchCount     = componentCount / SIMD_BATCH;

	viewBatch.Hydrate(fieldArrayTable, FlagBase);

	// Process batches
	for (uint32_t i = 0; i < batchCount; i++)
	{
		viewBatch.PrePhysics(dt);
		viewBatch.Advance(SIMD_BATCH);
	}

	// perform the last batch with a mask.
	alignas(32) typename T::MaskedType tailBatch;
	// Handle the tail with a mask
	tailBatch.Hydrate(fieldArrayTable, FlagBase, SIMD_BATCH * batchCount, componentCount % SIMD_BATCH);
	tailBatch.PrePhysics(dt);
}

template <typename T>
FORCE_INLINE void InvokeScalarUpdateImpl(double dt, void** fieldArrayTable, uint8_t* FlagBase, uint32_t componentCount)
{
	// Use Scalar for the update, this is where users can cross-reference entities and do non-SIMD things.
	alignas(32) T viewBatch;

	viewBatch.Hydrate(fieldArrayTable, FlagBase);

	// Process batches
	for (uint32_t i = 0; i < componentCount; i++)
	{
		viewBatch.ScalarUpdate(dt);
		viewBatch.Advance(1);
	}
}

template <typename T>
FORCE_INLINE void InvokePostPhysicsImpl(double dt, void** fieldArrayTable, uint8_t* FlagBase, uint32_t componentCount)
{
	alignas(32) typename T::WideType viewBatch;

	constexpr uint32_t SIMD_BATCH = 8;
	const uint32_t batchCount     = componentCount / SIMD_BATCH;

	viewBatch.Hydrate(fieldArrayTable, FlagBase);

	// Process batches
	for (uint32_t i = 0; i < batchCount; i++)
	{
		viewBatch.PostPhysics(dt);
		viewBatch.Advance(SIMD_BATCH);
	}

	TNX_ZONE_FINE_N("Tail Batch")
	// perform the last batch with a mask.
	alignas(32) typename T::MaskedType tailBatch;
	// Handle the tail with a mask
	tailBatch.Hydrate(fieldArrayTable, FlagBase, SIMD_BATCH * batchCount, componentCount % SIMD_BATCH);
	tailBatch.PostPhysics(dt);
}

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

	template <typename T>
	void RegisterPrefab()
	{
		const ClassID ID           = T::StaticClassID();
		EntityGetters[ID].ViewSize = sizeof(T);

		if constexpr (HasScalarUpdate<T>)
		{
			// Then in RegisterEntity:
			EntityGetters[ID].ScalarUpdate = InvokeScalarUpdateImpl<T>;
		}

		if constexpr (HasPrePhysics<T>)
		{
			// Then in RegisterEntity:
			EntityGetters[ID].PrePhys = InvokePrePhysicsImpl<T>;
		}

		if constexpr (HasPostPhysics<T>)
		{
			// Then in RegisterEntity:
			EntityGetters[ID].PostPhys = InvokePostPhysicsImpl<T>;
		}

		if constexpr (requires { T::kEntitiesPerChunk; })
		{
			EntityGetters[ID].EntitiesPerChunk = T::kEntitiesPerChunk;
		}
	}

	template <typename C, typename T>
	void RegisterPrefabComponent()
	{
		const ClassID ID             = C::StaticClassID();
		const ComponentTypeID TypeID = T::StaticTypeID();
		ComponentSignature& Def      = ClassToArchetype[ID];
		Def                          |= 1 << (TypeID - 1);

		for (auto& component : ClassToComponentList[ID])
		{
			if (component == TypeID) return;
		}

		ClassToComponentList[ID].push_back(TypeID);
		if constexpr (requires { T::SystemTypeID; }) ClassSystemID[ID] = ClassSystemID[ID] | T::SystemTypeID;

		// Store the per-slab slot index derived from StaticTemporalIndex().
		// StaticTemporalIndex() caches its result, so this is safe to call here even though
		// RegisterPrefabComponent may fire once per entity type that uses this component.
		if constexpr (requires { T::StaticTemporalIndex(); })
		{
			//ComponentFieldRegistry::Get().SetCacheSlotIndex(TypeID, T::StaticTemporalIndex());
		}
	}
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