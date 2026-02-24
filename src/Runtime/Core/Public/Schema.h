#pragma once
#include <functional>
#include <Logger.h>
#include <tuple>
#include <unordered_set>

#include "Profiler.h"
#include "Signature.h"
#include "Types.h"


// Helper concept to detect if T has a specific method
template <typename T> concept HasOnCreate = requires(T t) { t.OnCreate(); };
template <typename T> concept HasOnDestroy = requires(T t) { t.OnDestroy(); };
template <typename T> concept HasUpdate = requires(T t, double dt) { t.Update(dt); };
template <typename T> concept HasPrePhysics = requires(T t, double dt) { t.PrePhysics(dt); };
template <typename T> concept HasPostPhysics = requires(T t, double dt) { t.PostPhysics(dt); };
template <typename T> concept HasOnActivate = requires(T t) { t.OnActivate(); };
template <typename T> concept HasOnDeactivate = requires(T t) { t.OnDeactivate(); };
template <typename T> concept HasOnCollide = requires(T t) { t.OnCollide(); };
template <typename T> concept HasDefineSchema = requires(T t) { t.DefineSchema(); };
template <typename T> concept HasDefineFields = requires(T t) { t.DefineFields(); };

using UpdateFunc = void(*)(double, void**, uint32_t);

#define REGISTER_ENTITY_PREPHYS(Type, ClassID) \
    case ClassID: InvokePrePhysicsImpl<Type>(dt, fieldArrayTable, componentCount); break;

struct EntityMeta
{
    size_t ViewSize = 0;

    UpdateFunc PrePhys = nullptr;
    UpdateFunc PostPhys = nullptr;
    UpdateFunc Update = nullptr;

    EntityMeta(){}
    EntityMeta(const size_t inViewSize, const UpdateFunc prePhys, const UpdateFunc postPhys, const UpdateFunc update)
        : ViewSize(inViewSize)
        , PrePhys(prePhys)
        , PostPhys(postPhys)
        , Update(update)
    {}

    EntityMeta(const EntityMeta& rhs)
        : ViewSize(rhs.ViewSize)
        , PrePhys(rhs.PrePhys)
        , PostPhys(rhs.PostPhys)
        , Update(rhs.Update)
    {}
};

template <typename T>
__forceinline void InvokePrePhysicsImpl(double dt, void** fieldArrayTable, uint32_t componentCount)
{
    alignas(32) T viewBatch;

    constexpr uint32_t SIMD_BATCH = 8;
    const uint32_t batchCount = componentCount / SIMD_BATCH;

    viewBatch.Hydrate(fieldArrayTable);

    // Process batches
    for (uint32_t i = 0; i < batchCount; i++)
    {
        viewBatch.PrePhysics(dt);
        viewBatch.Advance(SIMD_BATCH);
    }

    // perform the last batch with a mask.
    alignas(32) typename T::MaskedType tailBatch;
    // Handle the tail with a mask
    tailBatch.Hydrate(fieldArrayTable, SIMD_BATCH * batchCount, componentCount % SIMD_BATCH);
    tailBatch.PrePhysics(dt);
    
}

template <typename T>
__forceinline void InvokeUpdateImpl(double dt, void** fieldArrayTable, uint32_t componentCount)
{
    alignas(32) T viewBatch;

    constexpr uint32_t SIMD_BATCH = 8;
    const uint32_t batchCount = componentCount / SIMD_BATCH;

    viewBatch.Hydrate(fieldArrayTable);

    // Process batches
    for (uint32_t i = 0; i < batchCount; i++)
    {
        viewBatch.Update(dt);
        viewBatch.Advance(SIMD_BATCH);
    }

    STRIGID_ZONE_FINE_N("Tail Batch")
    // perform the last batch with a mask.
    alignas(32) typename T::MaskedType tailBatch;
    // Handle the tail with a mask
    tailBatch.Hydrate(fieldArrayTable, SIMD_BATCH * batchCount, componentCount % SIMD_BATCH);
    tailBatch.Update(dt);
}

template <typename T>
__forceinline void InvokePostPhysicsImpl(double dt, void** fieldArrayTable, uint32_t componentCount)
{
    alignas(32) T viewBatch;

    constexpr uint32_t SIMD_BATCH = 8;
    const uint32_t batchCount = componentCount / SIMD_BATCH;

    viewBatch.Hydrate(fieldArrayTable);

    // Process batches
    for (uint32_t i = 0; i < batchCount; i++)
    {
        viewBatch.PostPhysics(dt);
        viewBatch.Advance(SIMD_BATCH);
    }

    STRIGID_ZONE_FINE_N("Tail Batch")
    // perform the last batch with a mask.
    alignas(32) typename T::MaskedType tailBatch;
    // Handle the tail with a mask
    tailBatch.Hydrate(fieldArrayTable, SIMD_BATCH * batchCount, componentCount % SIMD_BATCH);
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
    EntityMeta EntityGetters[4096];

    template <typename T>
    void RegisterPrefab()
    {
        const ClassID ID = T::StaticClassID();
        EntityGetters[ID].ViewSize = sizeof(T);

        if constexpr (HasUpdate<T>)
        {
            // Then in RegisterEntity:
            EntityGetters[ID].Update = InvokeUpdateImpl<T>;
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
    }

    template <typename C, typename T>
    void RegisterPrefabComponent()
    {
        const ClassID ID = C::StaticClassID();
        const ComponentTypeID TypeID = GetComponentTypeID<T>();
        ComponentSignature& Def = ClassToArchetype[ID];
        Def |= 1 << (TypeID - 1);
        
        for (auto& component : ClassToComponentList[ID])
        {
            if (component == TypeID) return;
        }
        
        ClassToComponentList[ID].push_back(TypeID);
    }
};

// The container for member pointers
template <typename... Members>
struct SchemaDefinition
{
    std::tuple<Members...> members;

    constexpr SchemaDefinition(Members... m) : members(m...)
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
