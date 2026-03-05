#pragma once
#include "Types.h"
#include <cstddef>

class Archetype; // Forward declaration (changed from struct to class)

struct Chunk
{
	static constexpr size_t DATA_SIZE           = CHUNK_SIZE;
	static constexpr size_t MAX_TEMPORAL_FIELDS = 64;                                         // Max temporal field arrays per archetype
	static constexpr size_t HEADER_SIZE         = 64 + (MAX_TEMPORAL_FIELDS * sizeof(void*)); // 64 base + pointers

	// Chunk header storing temporal field array pointers
	struct ChunkHeader
	{
		uint8_t TemporalFieldCount = 0;                   // Number of temporal fields used
		uint8_t _padding[7];                              // Align to 8 bytes
		void* TemporalFieldPointers[MAX_TEMPORAL_FIELDS]; // Absolute pointers to frame 0 data
	};

	alignas(64) ChunkHeader Header;
	alignas(64) uint8_t Data[DATA_SIZE - HEADER_SIZE]; // 64-byte alignment for cache line optimization

	inline uint8_t* GetBuffer(uint32_t Offset)
	{
		return Data + Offset;
	}

	// Get temporal field pointer (frame 0) by archetype index
	inline void* GetTemporalFieldPointer(uint8_t fieldIndex) const
	{
		if (fieldIndex >= Header.TemporalFieldCount) return nullptr;
		return Header.TemporalFieldPointers[fieldIndex];
	}

	// Set temporal field pointer (frame 0) by archetype index
	inline void SetTemporalFieldPointer(uint8_t fieldIndex, void* ptr)
	{
		if (fieldIndex < MAX_TEMPORAL_FIELDS)
		{
			Header.TemporalFieldPointers[fieldIndex] = ptr;
			if (fieldIndex >= Header.TemporalFieldCount) Header.TemporalFieldCount = fieldIndex + 1;
		}
	}
};
