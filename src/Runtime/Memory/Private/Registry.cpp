#include "Registry.h"
#include "Profiler.h"
#include <cassert>
#include <ranges>

#include "SchemaReflector.h"

Registry::Registry()
	: NextEntityIndex(1) // Start at 1 (0 is reserved for Invalid)
{
	STRIGID_ZONE_N("Registry::Constructor");
}

Registry::Registry(const EngineConfig* Config)
	: Registry()
{
	HistorySlab.Initialize(Config);
	// Reserve space for entity index
	EntityIndex.reserve(1024);
	InitializeArchetypes();
}

Registry::~Registry()
{
	LOG_INFO_F("Destroying Registry with %zu archetypes", Archetypes.size());

	for (auto& Pair : Archetypes)
	{
		LOG_INFO_F("Deleting archetype with %zu chunks", Pair.second->Chunks.size());
		delete Pair.second;
		LOG_INFO("Archetype deleted successfully");
	}
	Archetypes.clear();
}

Archetype* Registry::GetOrCreateArchetype(const Signature& Sig, const ClassID& ID)
{
	STRIGID_ZONE_C(STRIGID_COLOR_MEMORY);
	auto key = Archetype::ArchetypeKey(Sig, ID);

	// Check if archetype already exists
	auto It = Archetypes.find(key);
	if (It != Archetypes.end())
	{
		return It->second;
	}

	// Create new archetype
	auto NewArchetype = new Archetype(Sig, ID);

	// Build component layout from class ID
	std::vector<ComponentMetaEx> Components;
	MetaRegistry& MR = MetaRegistry::Get();

	auto compListIt = MR.ClassToComponentList.find(ID);
	if (compListIt != MR.ClassToComponentList.end())
	{
		ComponentFieldRegistry& CFR = ComponentFieldRegistry::Get();

		for (ComponentTypeID compTypeID : compListIt->second)
		{
			const std::vector<FieldMeta>* fields = CFR.GetFields(compTypeID);
			bool isDecomposed                    = (fields && !fields->empty());
			bool isTemporal                      = CFR.GetComponentMeta(compTypeID).IsTemporal;

			size_t componentSize = 0;
			if (isDecomposed && !fields->empty())
			{
				// Sum up field sizes for decomposed component
				for (const auto& field : *fields) componentSize += field.Size;
			}
			else
			{
				// For non-decomposed, we'd need the actual component size
				// For now, use a default
				componentSize = 64; // TODO: Get actual component size
			}

			Components.push_back(ComponentMetaEx{
				compTypeID,
				componentSize,
				FIELD_ARRAY_ALIGNMENT,
				0, // OffsetInChunk computed by BuildLayout
				isDecomposed,
				isTemporal,
				isDecomposed ? *fields : std::vector<FieldMeta>()
			});
		}
	}

	NewArchetype->BuildLayout(Components, &HistorySlab);

	Archetypes[key] = NewArchetype;
	return NewArchetype;
}

EntityID Registry::AllocateEntityID(uint16_t TypeID)
{
	STRIGID_ZONE_C(STRIGID_COLOR_MEMORY);

	EntityID Id;
	Id.Value = 0;

	// Try to reuse a free index
	if (!FreeIndices.empty())
	{
		uint32_t Index = FreeIndices.front();
		FreeIndices.pop();

		// Increment generation for recycled index
		uint16_t Generation = EntityIndex[Index].Generation + 1;
		if (Generation == 0) // Wrapped around
			Generation = 1;  // Skip 0 (reserved for invalid)

		Id.Index      = Index;
		Id.Generation = Generation;
		Id.TypeID     = TypeID;
		Id.OwnerID    = 0; // Server-owned by default
	}
	else
	{
		// Allocate new index
		Id.Index      = NextEntityIndex++;
		Id.Generation = 1; // First generation
		Id.TypeID     = TypeID;
		Id.OwnerID    = 0;
	}

	return Id;
}

void Registry::FreeEntityID(EntityID Id)
{
	STRIGID_ZONE_C(STRIGID_COLOR_MEMORY);

	uint32_t Index = Id.GetIndex();
	if (Index >= EntityIndex.size()) return;

	// Add to free list
	FreeIndices.push(Index);

	// Invalidate record
	EntityIndex[Index].Arch        = nullptr;
	EntityIndex[Index].TargetChunk = nullptr;
}

void Registry::Destroy(EntityID Id)
{
	STRIGID_ZONE_C(STRIGID_COLOR_MEMORY);
	// Defer destruction until end of frame
	PendingDestructions.push_back(Id);
}

bool Registry::DestroyRecord(EntityRecord& Record)
{
	// Find chunk index in archetype's chunk list
	Archetype* arch    = Record.Arch;
	Chunk* targetChunk = Record.TargetChunk;

	size_t chunkIndex = 0;
	bool foundChunk   = false;
	for (size_t i = 0; i < arch->Chunks.size(); ++i)
	{
		if (arch->Chunks[i] == targetChunk)
		{
			chunkIndex = i;
			foundChunk = true;
			break;
		}
	}

	if (!foundChunk)
	{
		LOG_ERROR_F("Failed to find chunk for entity %u during destruction", Record.Index);
		return true;
	}

	// Calculate where the last entity is BEFORE removal
	uint32_t lastEntityGlobalIndex = arch->TotalEntityCount - 1;
	uint32_t lastChunkIndex        = lastEntityGlobalIndex / arch->EntitiesPerChunk;
	uint32_t lastLocalIndex        = lastEntityGlobalIndex % arch->EntitiesPerChunk;

	// Check if we're actually swapping (not removing the last entity)
	bool willSwap = (chunkIndex != lastChunkIndex || Record.Index != lastLocalIndex);

	// Remove from archetype (swap-and-pop with last entity)
	arch->RemoveEntity(chunkIndex, Record.Index);

	// IMPORTANT: If we swapped with the last entity, update that entity's record
	if (willSwap)
	{
		// Find the entity that was swapped
		// We need to update EntityIndex for the swapped entity
		Chunk* lastChunk = arch->Chunks[lastChunkIndex];

		for (auto& entry : EntityIndex)
		{
			if (entry.Arch == arch &&
				entry.TargetChunk == lastChunk &&
				entry.Index == lastLocalIndex)
			{
				// Update this entity's record to point to the new location
				entry.TargetChunk = targetChunk;
				entry.Index       = Record.Index;
				break;
			}
		}
	}
	return true;
}

void Registry::ProcessDeferredDestructions()
{
	STRIGID_ZONE_C(STRIGID_COLOR_MEMORY);

	for (EntityID Id : PendingDestructions)
	{
		if (!Id.IsValid()) continue;

		uint32_t Index = Id.GetIndex();
		if (Index >= EntityIndex.size()) continue;

		EntityRecord& Record = EntityIndex[Index];

		// Validate generation
		if (Record.Generation != Id.GetGeneration()) continue;

		if (!Record.IsValid()) continue;

		if (DestroyRecord(Record))
		{
			// Free the entity ID
			FreeEntityID(Id);
		}
	}

	PendingDestructions.clear();

	STRIGID_PLOT("PendingDestructions", static_cast<double>(PendingDestructions.size()));

	LOG_INFO_F("Processed %zu deferred destructions. Existing Entities %u", PendingDestructions.size(), GetTotalEntityCount());
}

void Registry::InitializeArchetypes()
{
	MetaRegistry& MR            = MetaRegistry::Get();
	ComponentFieldRegistry& CFR = ComponentFieldRegistry::Get();
	// need to combine classes in the meta registry that have the same TypeID.

	for (auto& Arch : MR.ClassToArchetype)
	{
		auto key            = Archetype::ArchetypeKey(Arch.second, Arch.first);
		Archetype*& NewArch = Archetypes[key];
		if (!NewArch)
		{
			NewArch = new Archetype(key);
			std::vector<ComponentMetaEx> Components;
			for (auto& CompID : MR.ClassToComponentList[Arch.first])
			{
				Components.push_back(CFR.GetComponentMeta(CompID));
			}
			NewArch->BuildLayout(Components, &HistorySlab);
		}
	}
}

void Registry::ResetRegistry()
{
	// Index 0 is invalid, so drop first and then iterate backward.
	for (auto& Entity : EntityIndex | std::views::drop(1) | std::views::reverse)
	{
		if (!Entity.IsValid()) continue;
		DestroyRecord(Entity);
	}
	EntityIndex.clear();
	while (!FreeIndices.empty())
	{
		FreeIndices.pop();
	}
	PendingDestructions.clear();
	NextEntityIndex = 1;
}

uint32_t Registry::GetTotalChunkCount() const
{
	uint32_t totalChunks = 0;
	for (const auto& [sig, archetype] : Archetypes)
	{
		totalChunks += static_cast<uint32_t>(archetype->Chunks.size());
	}
	return totalChunks;
}

uint32_t Registry::GetTotalEntityCount() const
{
	uint32_t totalEntities = 0;
	for (const auto& [sig, archetype] : Archetypes)
	{
		totalEntities += archetype->TotalEntityCount;
	}
	return totalEntities;
}