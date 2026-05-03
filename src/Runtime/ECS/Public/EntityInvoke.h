#pragma once

#include "EntityMeta.h"
#include "Profiler.h"

// ---------------------------------------------------------------------------
// Entity invoke templates — wide SIMD + scalar + tail-masked dispatch.
//
// These are called via function pointers stored in EntityMeta. Each entity
// type that implements PrePhysics/PostPhysics/ScalarUpdate gets a concrete
// instantiation via PrefabReflector::RegisterPrefab.
// ---------------------------------------------------------------------------

template <typename T>
FORCE_INLINE void InvokePrePhysicsImpl(SimFloat dt, void** fieldArrayTable, void* FlagBase, uint32_t componentCount)
{
	alignas(32) typename T::WideType viewBatch;

	constexpr uint32_t SIMD_BATCH = 8;
	const uint32_t batchCount     = componentCount / SIMD_BATCH;

	viewBatch.Hydrate(fieldArrayTable, FlagBase);

	for (uint32_t i = 0; i < batchCount; i++)
	{
		viewBatch.PrePhysics(dt);
		viewBatch.Advance(SIMD_BATCH);
	}

	alignas(32) typename T::MaskedType tailBatch;
	tailBatch.Hydrate(fieldArrayTable, FlagBase, SIMD_BATCH * batchCount, componentCount % SIMD_BATCH);
	tailBatch.PrePhysics(dt);
}

template <typename T>
FORCE_INLINE void InvokeScalarUpdateImpl(SimFloat dt, void** fieldArrayTable, void* FlagBase, uint32_t componentCount)
{
	alignas(32) T viewBatch;

	viewBatch.Hydrate(fieldArrayTable, FlagBase);

	for (uint32_t i = 0; i < componentCount; i++)
	{
		viewBatch.ScalarUpdate(dt);
		viewBatch.Advance(1);
	}
}

template <typename T>
FORCE_INLINE void InvokeInitializeImpl(void** fieldArrayTable, void* FlagBase, uint32_t localIndex)
{
	T view;
	view.Hydrate(fieldArrayTable, FlagBase, localIndex);
	view.InitializeInternal();
}

template <typename T>
FORCE_INLINE void InvokePostPhysicsImpl(SimFloat dt, void** fieldArrayTable, void* FlagBase, uint32_t componentCount)
{
	alignas(32) typename T::WideType viewBatch;

	constexpr uint32_t SIMD_BATCH = 8;
	const uint32_t batchCount     = componentCount / SIMD_BATCH;

	viewBatch.Hydrate(fieldArrayTable, FlagBase);

	for (uint32_t i = 0; i < batchCount; i++)
	{
		viewBatch.PostPhysics(dt);
		viewBatch.Advance(SIMD_BATCH);
	}

	TNX_ZONE_FINE_N("Tail Batch")
	alignas(32) typename T::MaskedType tailBatch;
	tailBatch.Hydrate(fieldArrayTable, FlagBase, SIMD_BATCH * batchCount, componentCount % SIMD_BATCH);
	tailBatch.PostPhysics(dt);
}
