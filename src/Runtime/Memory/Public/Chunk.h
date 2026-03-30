#pragma once
#include "Types.h"
#include <cstddef>

class Archetype; // Forward declaration (changed from struct to class)

struct Chunk
{
	static constexpr size_t DATA_SIZE        = CHUNK_SIZE;
	static constexpr size_t MAX_CHUNK_FIELDS = 64;                                      // Max field arrays per archetype
	static constexpr size_t HEADER_SIZE      = 64 + (MAX_CHUNK_FIELDS * sizeof(void*)); // 64 base + pointers

	// Chunk header storing field array pointers
	struct ChunkHeader
	{
		uint8_t FieldCount     = 0;        // Number of fields used
		size_t CacheIndexStart = -1;       // The cache index for entities in this chunk -1 for no cache ID.
		void* FieldPtrs[MAX_CHUNK_FIELDS]; // Absolute pointers to frame 0 data
	};

	alignas(64) ChunkHeader Header;
	alignas(64) uint8_t Data[DATA_SIZE - HEADER_SIZE]; // 64-byte alignment for cache line optimization

	FORCE_INLINE uint8_t* GetBuffer(uint32_t offset)
	{
		return Data + offset;
	}

	FORCE_INLINE void* GetFieldPtr(size_t fieldIndex)
	{
		if (fieldIndex >= Header.FieldCount) return nullptr;
		return Header.FieldPtrs[fieldIndex];
	}

	// Set field pointer (frame 0) by archetype index
	FORCE_INLINE void SetFieldPointer(uint8_t fieldIndex, void* ptr)
	{
		if (fieldIndex < MAX_CHUNK_FIELDS)
		{
			Header.FieldPtrs[fieldIndex] = ptr;
			if (fieldIndex >= Header.FieldCount) Header.FieldCount = fieldIndex + 1;
		}
	}
};
