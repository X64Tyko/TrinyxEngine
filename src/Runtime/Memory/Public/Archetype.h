#pragma once
#include "Types.h"
#include "Signature.h"
#include "Chunk.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "ComponentView.h"
#include "FieldMeta.h"
#include "Schema.h"

// Archetype - manages storage for entities with a specific component signature
// Uses Structure-of-Arrays (SoA) layout within each chunk
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
	uint32_t EntitiesPerChunk = 1024; // How many entities fit in one chunk
	uint32_t TotalEntityCount = 0;    // Total entities across all chunks

	// Chunk storage
	std::vector<Chunk*> Chunks;

	// Component layout information
	std::unordered_map<ComponentTypeID, ComponentMetaEx> ComponentLayout;

	// Cached component iteration data (built once in BuildLayout)
	struct ComponentCacheEntry
	{
		ComponentTypeID TypeID;
		bool IsFieldDecomposed;
		size_t ChunkOffset;
	};

	std::vector<ComponentCacheEntry> ComponentIterationCache;

	// Get the number of entities in a specific chunk (handles tail chunk)
	uint32_t GetChunkCount(size_t ChunkIndex) const;

	// Slot descriptor returned by PushEntity (used by Registry::Create to update EntityIndex)
	struct EntitySlot
	{
		Chunk* TargetChunk;
		uint32_t LocalIndex;
		uint32_t CacheIndex; // Index across all chunks
	};

	std::vector<EntitySlot> ActiveEntitySlots;
	std::vector<EntitySlot> InactiveEntitySlots;

	// Get typed array pointer for a component in a specific chunk
	template <typename T>
	T* GetComponentArray(Chunk* TargetChunk, ComponentTypeID TypeID)
	{
		auto It = ComponentLayout.find(TypeID);
		if (It == ComponentLayout.end()) return nullptr;

		const ComponentMetaEx& Meta = It->second;
		return reinterpret_cast<T*>(TargetChunk->GetBuffer(static_cast<uint32_t>(Meta.OffsetInChunk)));
	}

	template <typename T>
	T* GetComponent(Chunk* TargetChunk, ComponentTypeID TypeID, uint32_t Index)
	{
		// TODO: need to verify this Index is valid
		return GetComponentArray<T>(TargetChunk, TypeID)[Index];
	}

	// Get field arrays for decomposed components (SoA)
	std::vector<void*> GetFieldArrays(Chunk* TargetChunk, ComponentTypeID TypeID);


	// Edge graph for archetype transitions (future optimization)
	std::unordered_map<ComponentTypeID, Archetype*> AddEdges;    // Add component X -> go to archetype Y
	std::unordered_map<ComponentTypeID, Archetype*> RemoveEdges; // Remove component X -> go to archetype Y

	// Field array lookup key
	struct FieldKey
	{
		ComponentTypeID componentID;
		uint8_t temporalFieldIndex;
		uint32_t fieldIndex;

		bool operator==(const FieldKey& other) const
		{
			return componentID == other.componentID && temporalFieldIndex == other.temporalFieldIndex && fieldIndex == other.fieldIndex;
		}
	};

	struct FieldKeyHash
	{
		size_t operator()(const FieldKey& key) const
		{
			return (static_cast<size_t>(key.componentID) << 32) | (static_cast<size_t>(key.temporalFieldIndex) << 16) | key.fieldIndex;
		}
	};

	// Storage for field array offsets
	std::unordered_map<FieldKey, size_t, FieldKeyHash> FieldOffsets;

	// Per-SoA-field info used by BuildFieldArrayTable and GetTemporalFieldWritePtr.
	struct TemporalFieldInfo
	{
		uint8_t SlotIndex;   // Index into chunk header temporal pointer array
		uint32_t FrameCount; // Number of frames in this field's slab ring
		size_t FrameStride;  // Bytes from frame N base to frame N+1 base
	};

	// Key: (ComponentTypeID, fieldIndex) → TemporalFieldInfo
	std::unordered_map<FieldKey, TemporalFieldInfo, FieldKeyHash> TemporalFieldIndices;

	// CACHED FIELD ARRAY TABLE - computed once after BuildLayout()
	// This stores the field array count and layout order
	struct FieldArrayDescriptor
	{
		uint8_t componentSlotIndex;
		ComponentTypeID componentID;
		uint32_t fieldIndex;
		uint8_t FieldSlotIndex;
		size_t fieldFrames;
		size_t frameStride;
		size_t size;
		bool isDecomposed;
		CacheTier tier;
		FieldValueType valueType = FieldValueType::Unknown;
	};

	std::vector<FieldArrayDescriptor> CachedFieldArrayLayout;
	size_t TotalFieldArrayCount = 0;

	// Pre-compute field array offsets (chunk-independent)
	// Call this once after BuildLayout() to cache offsets
	struct FieldArrayTemplate
	{
		size_t offsetInChunk;
		const char* debugName; // For debugging
	};

	std::vector<FieldArrayTemplate> FieldArrayTemplateCache;

	size_t TotalChunkDataSize = 0;

	// Get pointer to a specific field array within a chunk (always returns frame 0 for temporal)
	void* GetFieldArray(Chunk* chunk, ComponentTypeID typeID, uint32_t fieldIndex)
	{
		ComponentFieldRegistry& CFR = ComponentFieldRegistry::Get();
		FieldKey key{typeID, CFR.GetCacheSlotIndex(typeID), fieldIndex};

		// Check if this is a temporal field
		auto temporalIt = TemporalFieldIndices.find(key);
		if (temporalIt != TemporalFieldIndices.end())
		{
			// Return frame 0 pointer from chunk header
			return chunk->GetTemporalFieldPointer(temporalIt->second.SlotIndex);
		}

		// Regular chunk field
		auto it = FieldOffsets.find(key);
		if (it == FieldOffsets.end()) return nullptr;

		return chunk->GetBuffer(static_cast<uint32_t>(it->second));
	}

	// Build interleaved dual field array table (read T, write T+1) for FieldProxy::Bind()
	// Output layout: [read0, write0, read1, write1, read2, write2, ...]
	// absoluteFrame: raw monotonic frame counter — each SoA field computes its own
	// modular read/write indices from its FrameCount, so Volatile (3-frame triple-buffer) and
	// Temporal (N-frame) fields in the same archetype work correctly.
	void BuildFieldDualArrayTable(Chunk* chunk, void** outDualArrayTable, uint32_t absoluteFrame, uint32_t VolatileAbsoluteFrame) const
	{
		auto chunkBase = chunk->Data;

		size_t size = CachedFieldArrayLayout.size();
		for (size_t i = 0; i < size; ++i)
		{
			const auto& desc = CachedFieldArrayLayout[i];
			if (desc.isDecomposed)
			{
				if (desc.tier == CacheTier::None)
				{
					// Cold decomposed — single-frame, read and write are the same pointer
					void* ptr                    = chunkBase + FieldArrayTemplateCache[i].offsetInChunk;
					outDualArrayTable[i * 2]     = ptr;
					outDualArrayTable[i * 2 + 1] = ptr;
				}
				else
				{
					uint8_t* frame0Ptr = static_cast<uint8_t*>(chunk->GetTemporalFieldPointer(desc.fieldIndex));
					uint32_t readIdx   = (desc.tier == CacheTier::Temporal ? absoluteFrame : VolatileAbsoluteFrame) % desc.fieldFrames;
					uint32_t writeIdx  = ((desc.tier == CacheTier::Temporal ? absoluteFrame : VolatileAbsoluteFrame) + 1) % desc.fieldFrames;

					outDualArrayTable[i * 2]     = frame0Ptr + readIdx * desc.frameStride;  // Read T
					outDualArrayTable[i * 2 + 1] = frame0Ptr + writeIdx * desc.frameStride; // Write T+1
				}
			}
			else
			{
				// Non-decomposed — read and write are the same pointer
				void* ptr                    = chunkBase + FieldArrayTemplateCache[i].offsetInChunk;
				outDualArrayTable[i * 2]     = ptr;
				outDualArrayTable[i * 2 + 1] = ptr;
			}
		}
	}

	void BuildFieldArrayTable(Chunk* chunk, void** outFieldArrayTable, uint32_t absoluteFrame, uint32_t VolatileAbsoluteFrame) const
	{
		for (size_t i = 0; i < CachedFieldArrayLayout.size(); ++i)
		{
			const auto& desc = CachedFieldArrayLayout[i];

			if (desc.isDecomposed)
			{
				if (desc.tier == CacheTier::None)
				{
					// Cold decomposed — single-frame SoA array in chunk data
					outFieldArrayTable[i] = chunk->Data + FieldArrayTemplateCache[i].offsetInChunk;
				}
				else
				{
					// Temporal/Volatile — multi-frame SoA in slab
					size_t frameIdx       = ((desc.tier == CacheTier::Temporal ? absoluteFrame : VolatileAbsoluteFrame) % desc.fieldFrames) * desc.frameStride;
					outFieldArrayTable[i] = static_cast<uint8_t*>(chunk->GetTemporalFieldPointer(desc.FieldSlotIndex)) + frameIdx;
				}
			}
			else
			{
				// Non-decomposed component - get whole component array
				outFieldArrayTable[i] = chunk->Data + FieldArrayTemplateCache[i].offsetInChunk;
			}
		}
	}

	// Get total field array count (for allocating table)
	size_t GetFieldArrayCount() const
	{
		return TotalFieldArrayCount;
	}

	// Validate that cached layout matches current state
	bool ValidateCache() const
	{
		return CachedFieldArrayLayout.size() == TotalFieldArrayCount &&
			FieldArrayTemplateCache.size() == TotalFieldArrayCount;
	}

	// Get component type at specific table index (for debug/validation)
	ComponentTypeID GetComponentTypeAtTableIndex(size_t tableIndex) const
	{
		if (tableIndex >= CachedFieldArrayLayout.size()) return 0;

		return CachedFieldArrayLayout[tableIndex].componentID;
	}

	// Get field name at specific table index (for debugging)
	const char* GetFieldNameAtTableIndex(size_t tableIndex) const
	{
		if (tableIndex >= FieldArrayTemplateCache.size()) return "invalid";

		return FieldArrayTemplateCache[tableIndex].debugName;
	}

	// Helper: Align offset to alignment requirement
	static size_t AlignOffset(size_t offset, size_t alignment)
	{
		return (offset + alignment - 1) & ~(alignment - 1);
	}

	// Legacy: Get component array for non-decomposed components
	void* GetComponentArrayRaw(Chunk* chunk, ComponentTypeID typeID)
	{
		auto it = ComponentLayout.find(typeID);
		if (it == ComponentLayout.end()) return nullptr;

		return chunk->GetBuffer(static_cast<uint32_t>(it->second.OffsetInChunk));
	}

private:
	friend class Registry;

	class Registry* Reg = nullptr;

	SystemID ArchSystemID = SystemID::None;

	// Entity slot allocation / removal — only Registry drives these.
	void PushEntities(std::vector<EntitySlot>& outSlots, size_t count = 1);
	void RemoveEntity(size_t ChunkIndex, uint32_t LocalIndex, uint32_t ArchetypeIdx);

	// Layout construction — called once by Registry after component metadata is known.
	void BuildLayout(class Registry* reg, const std::vector<ComponentMetaEx>& Components, SystemID inArchSystemID = SystemID::None);

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
		// Start with classID
		size_t hash = key.ID;

		// Mix in signature using FNV-1a
		constexpr size_t FNV_PRIME  = 0x100000001b3;
		constexpr size_t FNV_OFFSET = 0xcbf29ce484222325;

		hash = FNV_OFFSET;
		hash ^= key.ID;
		hash *= FNV_PRIME;

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