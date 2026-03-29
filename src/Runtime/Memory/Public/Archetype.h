#pragma once
#include "Types.h"
#include "Signature.h"
#include "Chunk.h"
#include <span>
#include <unordered_map>
#include <vector>
#include "ComponentView.h"
#include "FieldMeta.h"
#include "FlatMap.h"
#include "Schema.h"

// Archetype — manages storage for all entities sharing a (Signature, ClassID) pair.
//
// Each archetype owns a list of Chunks (dense, 64-byte aligned allocations) and a
// ArchetypeFieldLayout table (FlatMap<FieldKey, FieldDescriptor>) that is the single
// source of truth for every field's slot index, cache tier, frame count, and stride.
//
// Fields may live in three places depending on their CacheTier:
//   Temporal/Volatile — SoA ring buffer in the corresponding ComponentCache slab.
//                       FieldPtrs[] in the chunk header point into the slab.
//   Cold (None)       — SoA arrays packed directly inside the chunk allocation.
//
// BuildFieldArrayTable / BuildFieldDualArrayTable resolve frame indices using pure
// arithmetic: `base + (frame % frameCount) * frameStride`. Cold fields degenerate
// to `base + 0` (frameCount=1, frameStride=0) — no tier branching needed.
//
// Lifetime: Registry owns all Archetype instances. Only Registry calls BuildLayout,
// PushEntities, and RemoveEntity.
class Archetype
{
public:
	struct ArchetypeKey
	{
		Signature Sig;
		ClassID ID;

		bool operator==(const ArchetypeKey& other) const
		{
			return ID == other.ID && Sig == other.Sig;
		}

		// Required for FlatMap (sorted container)
		bool operator<(const ArchetypeKey& other) const
		{
			// Compare ClassID first (likely fewer unique values, better branch prediction)
			if (ID != other.ID) return ID < other.ID;
			// Then compare signature bitset
			return Sig < other.Sig;
		}
	};

	Archetype(const Signature& Sig, const ClassID& ID, const char* DebugName = "Archetype");
	Archetype(const ArchetypeKey& ArchKey, const char* DebugName = "Archetype");
	~Archetype();

	// Component signature
	Signature ArchSignature;

	// ClassID - needed for using the correct entity during Hydration
	ClassID ArchClassID;

	// Debug name for profiling
	const char* DebugName;

	// Entity capacity and tracking
	uint32_t EntitiesPerChunk     = 0; // Set by BuildLayout from EntityMeta
	uint32_t AllocatedEntityCount = 0; // High-water mark of allocated slots (includes tombstoned).
	// Used by GetAllocatedChunkCount for iteration bounds.
	uint32_t TotalEntityCount = 0; // Live (non-tombstoned) entities across all chunks.

	// Chunk storage
	std::vector<Chunk*> Chunks;

	// Returns allocated slot count for a chunk (includes tombstoned). Use for iteration bounds —
	// callers rely on bitplane/masked-store to skip dead slots.
	uint32_t GetAllocatedChunkCount(size_t ChunkIndex) const;

	// Returns live (non-tombstoned) entity count for a chunk.
	uint32_t GetLiveChunkCount(size_t ChunkIndex) const;

	// Slot descriptor returned by PushEntities — gives Registry everything it needs
	// to populate an EntityRecord after allocating an archetype slot.
	struct EntitySlot
	{
		Chunk* TargetChunk;  // The chunk this slot belongs to
		uint32_t ChunkIndex; // Which chunk in Chunks[] (0-based)
		uint32_t LocalIndex; // Index within the chunk (0 .. EntitiesPerChunk-1)
		uint32_t ArchIndex;  // Flat index across all chunks in this archetype
		uint32_t CacheIndex; // Index in the temporal/volatile cache slab
	};

	std::vector<EntitySlot> ActiveEntitySlots;
	std::vector<EntitySlot> InactiveEntitySlots;

	// Get all field array base pointers for a given component type within a chunk (frame 0).
	std::vector<void*> GetFieldArrays(Chunk* TargetChunk, ComponentTypeID TypeID);

	// Edge graph for archetype transitions (future optimization)
	std::unordered_map<ComponentTypeID, Archetype*> AddEdges;    // Add component X -> go to archetype Y
	std::unordered_map<ComponentTypeID, Archetype*> RemoveEdges; // Remove component X -> go to archetype Y

	// Composite key for field lookup: (componentID, cacheSlotIndex, fieldIndex)
	struct FieldKey
	{
		ComponentTypeID componentID;
		uint8_t cacheSlotIndex;
		uint32_t fieldIndex;

		bool operator==(const FieldKey& other) const
		{
			return componentID == other.componentID && cacheSlotIndex == other.cacheSlotIndex && fieldIndex == other.fieldIndex;
		}

		bool operator<(const FieldKey& other) const { return pack() < other.pack(); }

		size_t pack() const
		{
			return (static_cast<size_t>(componentID) << 32) | (static_cast<size_t>(cacheSlotIndex) << 16) | fieldIndex;
		}
	};

	struct FieldDescriptor
	{
		uint8_t fieldSlotIndex;         // Index into Chunk::Header::FieldPtrs[]
		uint8_t componentSlotIndex;     // Field index within its component (0, 1, 2...)
		uint8_t temporalComponentIndex; // Cache slot index in the temporal/volatile slab
		ComponentTypeID componentID;    // Component type this field belongs to
		CacheTier tier;                 // Which cache tier (Temporal, Volatile, None)
		FieldValueType valueType = FieldValueType::Unknown;
		size_t fieldSize;        // Size of one element (e.g. 4 for float)
		size_t fieldFrameCount;  // Frame count in cache ring (1 for cold)
		size_t fieldFrameStride; // Bytes between frame N and frame N+1 (0 for cold)
		bool bIsTemporal;        // True if stored in temporal/volatile slab
	};

	// Single source of truth for every field in this archetype — maps (component, cacheSlot, fieldIdx)
	// to its chunk slot index, cache tier, frame ring parameters, and element size.
	FlatMap<FieldKey, FieldDescriptor> ArchetypeFieldLayout;

	// Total byte size of each chunk allocation (header + all cold field arrays). Set by BuildLayout.
	size_t TotalChunkDataSize = 0;

	// Get base pointer to a field array within a chunk (frame 0 for temporal/volatile fields).
	void* GetFieldArray(Chunk* chunk, ComponentTypeID typeID, uint32_t fieldIndex)
	{
		FieldKey key{typeID, ComponentFieldRegistry::Get().GetCacheSlotIndex(typeID), fieldIndex};
		auto* desc = ArchetypeFieldLayout.find(key);
		return desc ? chunk->GetFieldPtr(desc->fieldSlotIndex) : nullptr;
	}

	// Build interleaved dual field array table (read T, write T+1) for FieldProxy::Bind()
	// Output layout: [read0, write0, read1, write1, read2, write2, ...]
	// absoluteFrame/VolatileAbsoluteFrame: raw monotonic frame counters — each field computes
	// its own modular read/write indices from its FrameCount, so Volatile (triple-buffer) and
	// Temporal (N-frame) fields in the same archetype work correctly.
	// Cold fields use frameCount=1, frameStride=0 so the math degenerates to base+0 (branchless).
	void BuildFieldDualArrayTable(Chunk* chunk, void** outDualArrayTable, uint32_t absoluteFrame, uint32_t VolatileAbsoluteFrame) const
	{
		for (const auto& [fkey, fdesc] : ArchetypeFieldLayout)
		{
			size_t idx        = fdesc.fieldSlotIndex;
			auto* base        = static_cast<uint8_t*>(chunk->Header.FieldPtrs[idx]);
			uint32_t frame    = (fdesc.tier == CacheTier::Temporal ? absoluteFrame : VolatileAbsoluteFrame);
			uint32_t readIdx  = frame % fdesc.fieldFrameCount;
			uint32_t writeIdx = (frame + 1) % fdesc.fieldFrameCount;

			outDualArrayTable[idx * 2]     = base + readIdx * fdesc.fieldFrameStride;
			outDualArrayTable[idx * 2 + 1] = base + writeIdx * fdesc.fieldFrameStride;
		}
	}

	// Build single field array table for a specific frame (used by update dispatch, serialization).
	// Cold fields use frameCount=1, frameStride=0 so the math degenerates to base+0 (branchless).
	void BuildFieldArrayTable(Chunk* chunk, void** outFieldArrayTable, uint32_t absoluteFrame, uint32_t VolatileAbsoluteFrame) const
	{
		for (const auto& [fkey, fdesc] : ArchetypeFieldLayout)
		{
			size_t idx              = fdesc.fieldSlotIndex;
			auto* base              = static_cast<uint8_t*>(chunk->Header.FieldPtrs[idx]);
			uint32_t frame          = (fdesc.tier == CacheTier::Temporal ? absoluteFrame : VolatileAbsoluteFrame);
			outFieldArrayTable[idx] = base + (frame % fdesc.fieldFrameCount) * fdesc.fieldFrameStride;
		}
	}

	size_t GetFieldArrayCount() const { return ArchetypeFieldLayout.count(); }

private:
	friend class Registry;

	class Registry* Reg = nullptr;

	SystemID ArchSystemID = SystemID::None;

	// Entity slot allocation / removal — only Registry drives these.
	void PushEntities(std::span<EntitySlot> outSlots);
	void RemoveEntity(size_t ChunkIndex, uint32_t LocalIndex, uint32_t ArchetypeIdx);

	// Layout construction — called once by Registry after component metadata is known.
	void BuildLayout(class Registry* reg, const std::vector<ComponentMetaEx>& Components, SystemID inArchSystemID = SystemID::None);

	static size_t AlignOffset(size_t offset, size_t alignment)
	{
		return (offset + alignment - 1) & ~(alignment - 1);
	}

	// Chunk allocation
	Chunk* AllocateChunk();

	// Free all chunks (used by ResetRegistry so re-spawn goes through AllocateChunk
	// which properly re-allocates slab field arrays with correct allocator offsets).
	void FreeAllChunks();
};

struct ArchetypeKeyHash
{
	size_t operator()(const Archetype::ArchetypeKey& key) const
	{
		// FNV-1a hash
		constexpr size_t FNV_PRIME  = 0x100000001b3;
		constexpr size_t FNV_OFFSET = 0xcbf29ce484222325;

		size_t hash = FNV_OFFSET;
		hash        ^= static_cast<uint64_t>(key.ID) << 48;
		hash        *= FNV_PRIME;

		// Process signature in 64-bit chunks
		const uint64_t* data = reinterpret_cast<const uint64_t*>(&key.Sig);
		for (size_t i = 0; i < MAX_COMPONENTS / 64; ++i)
		{
			// 256 bits = 4 × 64-bit chunks
			hash ^= data[i];
			hash *= FNV_PRIME;
		}

		return hash;
	}
};