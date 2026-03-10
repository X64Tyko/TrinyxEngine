#pragma once
#include <cstdint>

/**
 * SnapshotBuffer: 1:1 mapping with sparse array components
 *
 * This structure matches the layout we need for interpolation
 * and can be directly copied from Transform + ColorData
 */
struct alignas(16) SnapshotEntry
{
	// Transform data (52 bytes + 12 padding = 64 bytes)
	float PositionX, PositionY, PositionZ;
	float RotQx, RotQy, RotQz, RotQw;
	float ScaleX, ScaleY, ScaleZ;
	float _pad0; // Padding to 64 bytes

	// ColorData (16 bytes)
	float ColorR, ColorG, ColorB, ColorA = 0.f;

	bool IsValid()
	{
		return ColorA != 0.0f;
	}
};

static_assert(sizeof(SnapshotEntry) == 64, "SnapshotEntry must be 64 bytes");

/**
 * Sparse array snapshot pointers
 *
 * RenderThread stores pointers to the render/physics sparse arrays
 * to quickly snapshot without ECS queries
 */
struct SparseArraySnapshot
{
	void* TransformArray = nullptr;
	void* ColorArray     = nullptr;
	uint32_t EntityCount = 0;
};