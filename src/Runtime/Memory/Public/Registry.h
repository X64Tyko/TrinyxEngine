#pragma once
#include <queue>
#include <unordered_map>
#include <vector>
#include "Archetype.h"
#include "EntityRecord.h"
#include "FieldMeta.h"
#include "Schema.h"
#include "SchemaReflector.h"
#include "Signature.h"
#include "TemporalComponentCache.h"
#include "Types.h"

struct EngineConfig;
// Registry - Central entity management system
// Handles entity creation, destruction, and component access
class Registry
{
public:
    Registry();
    Registry(const EngineConfig* Config);
    ~Registry();

    // Entity creation, Reflection allows this to be extremely quick
    // Usage: EntityID player = Registry::Get().Create<PlayerController>();
    template <typename T>
    EntityID Create();

    // Destroy an entity (deferred until end of frame)
    void Destroy(EntityID Id);
    bool DestroyRecord(EntityRecord& Record);

    // Get component from entity
    template <typename T>
    T* GetComponent(EntityID Id);

    // Check if entity has component
    template <typename T>
    bool HasComponent(EntityID Id);

    // Get or create archetype for a given signature
    Archetype* GetOrCreateArchetype(const Signature& Sig, const ClassID& ID);

    // Apply all pending destructions (called at end of frame)
    void ProcessDeferredDestructions();

    template <typename... Components>
    std::vector<Archetype*> ComponentQuery();
    
    template <typename... Classes>
    std::vector<Archetype*> ClassQuery();

    // Invoke all lifecycle functions of a specific type
    // currentFrame: frame T (read from T, write to T+1)
    void InvokeScalarUpdate(double dt, uint32_t currentFrame);
    void InvokePrePhys(double dt, uint32_t currentFrame);
    void InvokePostPhys(double dt, uint32_t currentFrame);

    // Access temporal cache
    TemporalComponentCache* GetTemporalCache() { return &HistorySlab; }

    // Memory diagnostics
    uint32_t GetTotalChunkCount() const;
    uint32_t GetTotalEntityCount() const;

    // Resets the registry to default, useful after tests.
    // TODO: this needs to not be public.
    void ResetRegistry();

private:
    // Initialize archetypes with data from MetaRegistry
    void InitializeArchetypes();

    // Global entity lookup table (indexed by EntityID.GetIndex())
    std::vector<EntityRecord> EntityIndex;

    // Free list for recycled entity indices
    std::queue<uint32_t> FreeIndices;

    // Next entity index to allocate (if free list is empty)
    uint32_t NextEntityIndex = 0;

    // Archetype storage (pair<signature, classID> → archetype)
    std::unordered_map<Archetype::ArchetypeKey, Archetype*, ArchetypeKeyHash> Archetypes;

    // Pending destructions (processed at end of frame)
    std::vector<EntityID> PendingDestructions;

    TemporalComponentCache HistorySlab;

    // Allocate a new EntityID
    EntityID AllocateEntityID(uint16_t TypeID);

    // Free an EntityID (returns index to free list)
    void FreeEntityID(EntityID Id);

    // Helper: Build signature from component list
    template <typename... Components>
    Signature BuildSignature();
};

// Template implementations must be in header

template <typename T>
EntityID Registry::Create()
{
    // Static local caching - archetype is calculated once per type T
    static Archetype* CachedArchetype = nullptr;
    static bool Initialized = false;

    if (!Initialized)
    {
        ClassID classID = T::StaticClassID();
        MetaRegistry& MR = MetaRegistry::Get();

#ifdef _DEBUG // || _WITH_EDITOR
        // Runtime guard: Check if entity type was registered with STRIGID_REGISTER_ENTITY
        if (MR.ClassToArchetype.find(classID) == MR.ClassToArchetype.end())
        {
            // FATAL: Entity type not registered
            const char* typeName = typeid(T).name();
            LOG_ERROR_F("FATAL: Entity type '%s' not registered! Did you forget STRIGID_REGISTER_ENTITY(%s)?",
                        typeName, typeName);

        // In debug builds, assert. In release, fail gracefully
#ifdef _DEBUG
        assert(false && "Entity type not registered - add STRIGID_REGISTER_ENTITY macro");
#endif

        // Return invalid entity ID
        return EntityID{};
        }
#endif

        Signature Sig = MR.ClassToArchetype[classID];

        CachedArchetype = GetOrCreateArchetype(Sig, classID);
        Initialized = true;
    }

    // Allocate entity ID
    EntityID Id = AllocateEntityID(T::StaticClassID());

    // Allocate slot in archetype
    Archetype::EntitySlot Slot = CachedArchetype->PushEntity();

    // Update EntityIndex
    uint32_t Index = Id.GetIndex();
    if (Index >= EntityIndex.size())
    {
        EntityIndex.resize(Index * 2);
    }

    EntityRecord& Record = EntityIndex[Index];
    Record.Arch = CachedArchetype;
    Record.TargetChunk = Slot.TargetChunk;
    Record.Index = static_cast<uint16_t>(Slot.LocalIndex);
    Record.Generation = Id.GetGeneration();

    return Id;
}

template <typename T>
T* Registry::GetComponent(EntityID Id)
{
    if (!Id.IsValid())
        return nullptr;

    uint32_t Index = Id.GetIndex();
    if (Index >= EntityIndex.size())
        return nullptr;

    EntityRecord& Record = EntityIndex[Index];

    // Validate generation (detect use-after-free)
    if (Record.Generation != Id.GetGeneration())
        return nullptr;

    if (!Record.IsValid())
        return nullptr;

    // TODO: Get ComponentTypeID from reflection (Week 5)
    ComponentTypeID TypeID = GetComponentTypeID<T>();

    // Get component array from archetype
    T* ComponentArray = Record.Arch->GetComponentArray<T>(Record.TargetChunk, TypeID);
    if (!ComponentArray)
        return nullptr;

    // Return pointer to this entity's component
    return &ComponentArray[Record.Index];
}

template <typename T>
bool Registry::HasComponent(EntityID Id)
{
    return GetComponent<T>(Id) != nullptr;
}

template <typename... Components>
Signature Registry::BuildSignature()
{
    Signature Sig;
    // Fold expression to set all component bits
    ((Sig.Set(GetComponentTypeID<Components>() - 1)), ...);
    return Sig;
}

template <typename... Components>
std::vector<Archetype*> Registry::ComponentQuery()
{
    std::vector<Archetype*> Results(Archetypes.size());
    uint8_t ArchIdx = 0;
    bool Valid = false;
    Signature Sig = BuildSignature<Components...>();
    for (auto Arch : Archetypes)
    {
        Valid = Arch.first.Sig.Contains(Sig);
        Results[ArchIdx] = Arch.second;
        ArchIdx += !!Valid;
    }

    Results.erase(Results.begin() + ArchIdx, Results.end());
    return Results;
}

template <typename... Classes>
std::vector<Archetype*> Registry::ClassQuery()
{
    std::vector<Archetype*> Results(Archetypes.size());
    std::unordered_set<ClassID> ClassIDs{Classes::StaticClassID()...};
    uint8_t ArchIdx = 0;
    bool Valid = false;
    for (auto Arch : Archetypes)
    {
        Valid = ClassIDs.contains(Arch.first.ID);
        Results[ArchIdx] = Arch.second;
        ArchIdx += !!Valid;
    }

    Results.erase(Results.begin() + ArchIdx, Results.end());
    return Results;
}

inline void Registry::InvokeScalarUpdate(double dt, uint32_t currentFrame)
{
    STRIGID_ZONE_C(STRIGID_COLOR_LOGIC);

    // Lock frame T+1 for writing (with wrapping)
    uint32_t writeFrame = HistorySlab.GetNextFrame(currentFrame);
    uint32_t readFrameIdx = HistorySlab.GetFrameIndex(currentFrame);
    if (!HistorySlab.TryLockFrameForWrite(writeFrame))
    {
        LOG_ERROR_F("Failed to acquire write lock on frame %u", writeFrame);
        return;
    }

    // Verify frame T is readable
    if (!HistorySlab.VerifyFrameReadable(currentFrame))
    {
        LOG_ERROR_F("Frame %u is not readable (locked by another thread)", currentFrame);
        HistorySlab.UnlockFrameWrite(writeFrame);
        return;
    }

    constexpr size_t MAX_FIELD_ARRAYS = 64; // Max total fields across all components in archetype
    void* dualArrayTable[MAX_FIELD_ARRAYS * 2]; // Interleaved read/write for FieldProxy::Bind()

    for (auto& [sig, arch] : Archetypes)
    {
        UpdateFunc ScalarUpdate = MetaRegistry::Get().EntityGetters[sig.ID].ScalarUpdate;
        if (!ScalarUpdate)
            continue;

        size_t size = arch->Chunks.size();
        for (size_t chunkIdx = 0; chunkIdx < size; ++chunkIdx)
        {
            Chunk* chunk = arch->Chunks[chunkIdx];
            uint32_t entityCount = arch->GetChunkCount(chunkIdx);

            if (entityCount == 0)
                continue;

            // Build interleaved dual array table (read T, write T+1)
            arch->BuildFieldArrayTable(chunk, dualArrayTable, readFrameIdx, writeFrame);

            // Invoke batch processor with dual array table
            ScalarUpdate(dt, dualArrayTable, entityCount);
        }
    }

    // Release locks at end of update
    HistorySlab.UnlockFrameWrite(writeFrame);
}

inline void Registry::InvokePrePhys(double dt, uint32_t currentFrame)
{
    STRIGID_ZONE_C(STRIGID_COLOR_LOGIC);

    // Lock frame T+1 for writing (with wrapping)
    uint32_t writeFrame = HistorySlab.GetNextFrame(currentFrame);
    uint32_t readFrameIdx = HistorySlab.GetFrameIndex(currentFrame);
    if (!HistorySlab.TryLockFrameForWrite(writeFrame))
    {
        LOG_ERROR_F("Failed to acquire write lock on frame %u", writeFrame);
        return;
    }

    // Verify frame T is readable
    if (!HistorySlab.VerifyFrameReadable(currentFrame))
    {
        LOG_ERROR_F("Frame %u is not readable (locked by another thread)", currentFrame);
        HistorySlab.UnlockFrameWrite(writeFrame);
        return;
    }

    constexpr size_t MAX_FIELD_ARRAYS = 64; // Max total fields across all components in archetype
    void* dualArrayTable[MAX_FIELD_ARRAYS * 2]; // Interleaved read/write for FieldProxy::Bind()

    for (auto& [sig, arch] : Archetypes)
    {
        UpdateFunc prePhys = MetaRegistry::Get().EntityGetters[sig.ID].PrePhys;
        if (!prePhys)
            continue;

        size_t size = arch->Chunks.size();
        for (size_t chunkIdx = 0; chunkIdx < size; ++chunkIdx)
        {
            Chunk* chunk = arch->Chunks[chunkIdx];
            uint32_t entityCount = arch->GetChunkCount(chunkIdx);

            if (entityCount == 0)
                continue;

            // Build interleaved dual array table (read T, write T+1)
            arch->BuildFieldArrayTable(chunk, dualArrayTable, readFrameIdx, writeFrame);

            // Invoke batch processor with dual array table
            prePhys(dt, dualArrayTable, entityCount);
        }
    }
    
    // Release locks at end of update
    HistorySlab.UnlockFrameWrite(writeFrame);
}

inline void Registry::InvokePostPhys(double dt, uint32_t currentFrame)
{
    STRIGID_ZONE_C(STRIGID_COLOR_LOGIC);

    // Lock frame T+1 for writing (with wrapping)
    uint32_t writeFrame = HistorySlab.GetNextFrame(currentFrame);
    uint32_t readFrameIdx = HistorySlab.GetFrameIndex(currentFrame);
    if (!HistorySlab.TryLockFrameForWrite(writeFrame))
    {
        LOG_ERROR_F("Failed to acquire write lock on frame %u", writeFrame);
        return;
    }

    // Verify frame T is readable
    if (!HistorySlab.VerifyFrameReadable(currentFrame))
    {
        LOG_ERROR_F("Frame %u is not readable (locked by another thread)", currentFrame);
        HistorySlab.UnlockFrameWrite(writeFrame);
        return;
    }
    
    constexpr size_t MAX_FIELD_ARRAYS = 64; // Max total fields across all components in archetype
    void* dualArrayTable[MAX_FIELD_ARRAYS * 2]; // Interleaved read/write for FieldProxy::Bind()

    for (auto& [sig, arch] : Archetypes)
    {
        UpdateFunc PostPhys = MetaRegistry::Get().EntityGetters[sig.ID].PostPhys;
        if (!PostPhys)
            continue;

        size_t size = arch->Chunks.size();
        for (size_t chunkIdx = 0; chunkIdx < size; ++chunkIdx)
        {
            Chunk* chunk = arch->Chunks[chunkIdx];
            uint32_t entityCount = arch->GetChunkCount(chunkIdx);

            if (entityCount == 0)
                continue;

            // Build interleaved dual array table (read T, write T+1)
            arch->BuildFieldArrayTable(chunk, dualArrayTable, readFrameIdx, writeFrame);

            // Invoke batch processor with dual array table
            PostPhys(dt, dualArrayTable, entityCount);
        }
    }
    
    // Release locks at end of update
    HistorySlab.UnlockFrameWrite(writeFrame);
}
