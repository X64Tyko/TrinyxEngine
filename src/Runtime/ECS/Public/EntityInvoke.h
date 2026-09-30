#pragma once

#include "TemporalFlagBits.h"
#include "EntityMeta.h"
#include "MemoryDefines.h"
#include "Profiler.h"

/// @brief Entity dispatch templates — wide SIMD + scalar + tail-masked loops.
///
/// Called via function pointers in @ref EntityMeta. Each entity type that
/// implements @c PrePhysics / @c PostPhysics / @c ScalarUpdate gets a concrete
/// instantiation registered by @c PrefabReflector::RegisterPrefab.

/// @brief Wide AVX2 pre-physics sweep: 8-wide batches + masked tail.
/// @param dt Fixed timestep.
/// @param fieldArrayTable Per-field SoA array pointers from the archetype chunk.
/// @param FlagBase Pointer to the flags array (CacheSlotMeta::Flags).
/// @param componentCount Total entity count in this archetype chunk.
template <typename T>
FORCE_INLINE void InvokePrePhysicsImpl(SimFloat dt, void** fieldArrayTable, void* FlagBase, uint32_t componentCount)
{
	alignas(FIELD_ARRAY_ALIGNMENT) typename T::WideType viewBatch;

	constexpr uint32_t SIMD_BATCH = 8;
	const uint32_t batchCount     = componentCount / SIMD_BATCH;

	viewBatch.Hydrate(fieldArrayTable, FlagBase);

	for (uint32_t i = 0; i < batchCount; i++)
	{
		viewBatch.PrePhysics(dt);
		viewBatch.Advance(SIMD_BATCH);
	}

	alignas(FIELD_ARRAY_ALIGNMENT) typename T::MaskedType tailBatch;
	tailBatch.Hydrate(fieldArrayTable, FlagBase, SIMD_BATCH * batchCount, componentCount % SIMD_BATCH);
	tailBatch.PrePhysics(dt);
}

/// @brief Scalar per-entity update loop (outside fixed step, no SIMD).
template <typename T>
FORCE_INLINE void InvokeScalarUpdateImpl(SimFloat dt, void** fieldArrayTable, void* FlagBase, uint32_t componentCount)
{
	alignas(FIELD_ARRAY_ALIGNMENT) T viewBatch;

	viewBatch.Hydrate(fieldArrayTable, FlagBase);

	for (uint32_t i = 0; i < componentCount; i++)
	{
		viewBatch.ScalarUpdate(dt);
		viewBatch.Advance(1);
	}
}

/// @brief Single-entity initialization at spawn time.
/// @param localIndex Slot index within the chunk's field arrays.
template <typename T>
FORCE_INLINE void InvokeInitializeImpl(void** fieldArrayTable, void* FlagBase, uint32_t localIndex)
{
	T view;
	view.Hydrate(fieldArrayTable, FlagBase, localIndex);
	view.InitializeInternal();
}

/// @brief Wide AVX2 post-physics sweep: 8-wide batches + masked tail.
template <typename T>
FORCE_INLINE void InvokePostPhysicsImpl(SimFloat dt, void** fieldArrayTable, void* FlagBase, uint32_t componentCount)
{
	alignas(FIELD_ARRAY_ALIGNMENT) typename T::WideType viewBatch;

	constexpr uint32_t SIMD_BATCH = 8;
	const uint32_t batchCount     = componentCount / SIMD_BATCH;

	viewBatch.Hydrate(fieldArrayTable, FlagBase);

	for (uint32_t i = 0; i < batchCount; i++)
	{
		viewBatch.PostPhysics(dt);
		viewBatch.Advance(SIMD_BATCH);
	}

	TNX_ZONE_FINE_N("Tail Batch")
	alignas(FIELD_ARRAY_ALIGNMENT) typename T::MaskedType tailBatch;
	tailBatch.Hydrate(fieldArrayTable, FlagBase, SIMD_BATCH * batchCount, componentCount % SIMD_BATCH);
	tailBatch.PostPhysics(dt);
}

#ifdef TNX_ENABLE_ROLLBACK
/// @brief Rollback resim pre-physics: scalar loop filtered to @c DirtiedFrame entities only.
///
/// Called in place of the wide SIMD variant during resimulation to skip the
/// majority of entities whose state is unchanged by the incoming correction.
template <typename T>
FORCE_INLINE void InvokePrePhysicsResimImpl(SimFloat dt, void** fieldArrayTable, void* FlagBase, uint32_t componentCount)
{
	if constexpr (HasPrePhysics<T>)
	{
		const int32_t* flags    = static_cast<const int32_t*>(FlagBase);
		constexpr int32_t dirty = static_cast<int32_t>(TemporalFlagBits::DirtiedFrame);
		alignas(FIELD_ARRAY_ALIGNMENT) T view;
		view.Hydrate(fieldArrayTable, FlagBase);
		for (uint32_t i = 0; i < componentCount; ++i)
		{
			if (flags[i] & dirty) view.PrePhysics(dt);
			view.Advance(1);
		}
	}
}

/// @brief Rollback resim post-physics: scalar loop filtered to @c DirtiedFrame entities only.
template <typename T>
FORCE_INLINE void InvokePostPhysicsResimImpl(SimFloat dt, void** fieldArrayTable, void* FlagBase, uint32_t componentCount)
{
	if constexpr (HasPostPhysics<T>)
	{
		const int32_t* flags    = static_cast<const int32_t*>(FlagBase);
		constexpr int32_t dirty = static_cast<int32_t>(TemporalFlagBits::DirtiedFrame);
		alignas(FIELD_ARRAY_ALIGNMENT) T view;
		view.Hydrate(fieldArrayTable, FlagBase);
		for (uint32_t i = 0; i < componentCount; ++i)
		{
			if (flags[i] & dirty) view.PostPhysics(dt);
			view.Advance(1);
		}
	}
}
#endif
