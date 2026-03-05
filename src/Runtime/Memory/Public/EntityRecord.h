#pragma once
#include <cstdint>

// Forward declarations
class Archetype; // Changed from struct to class
struct Chunk;

// Entity lookup table entry
// Maps EntityID.Index to actual memory location
struct EntityRecord
{
	Archetype* Arch       = nullptr; // Which archetype this entity belongs to
	Chunk* TargetChunk    = nullptr; // Which chunk within that archetype
	uint16_t Index        = 0;       // Index within the chunk
	uint32_t ArchetypeIdx = 0;       // Index at the archetype level
	uint16_t Generation   = 0;       // For validation (matches EntityID.Generation)

	// Check if this record is valid
	bool IsValid() const
	{
		return Arch != nullptr && TargetChunk != nullptr;
	}
};