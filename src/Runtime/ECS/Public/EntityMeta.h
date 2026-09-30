#pragma once

#include "Types.h"
#include "PhysicsTypes.h"

/// @defgroup EntityConcepts Entity lifecycle concepts
/// @{
/// Concept detection for entity lifecycle hooks.
/// Implement the method to register for the tick; omit it to pay nothing.
enum class SystemID : uint8_t;

template <typename T>
concept HasOnCreate = requires(T t) { t.OnCreate(); }; ///< Entity created.
template <typename T>
concept HasOnDestroy = requires(T t) { t.OnDestroy(); }; ///< Entity about to be destroyed.
template <typename T>
concept HasScalarUpdate = requires(T t, SimFloat dt) { t.ScalarUpdate(dt); }; ///< Once-per-frame scalar update (outside fixed step).
template <typename T>
concept HasPrePhysics = requires(T t, SimFloat dt) { t.PrePhysics(dt); }; ///< Fixed-step pre-physics SIMD sweep.
template <typename T>
concept HasPostPhysics = requires(T t, SimFloat dt) { t.PostPhysics(dt); }; ///< Fixed-step post-physics SIMD sweep.
template <typename T>
concept HasPhysicsStep = requires(T t, SimFloat dt) { t.PhysicsStep(dt); }; ///< Per-physics-substep Construct tick (CharacterVirtual).
template <typename T>
concept HasOnActivate = requires(T t) { t.OnActivate(); }; ///< Alive→Active transition.
template <typename T>
concept HasOnDeactivate = requires(T t) { t.OnDeactivate(); }; ///< Active→Inactive transition.
template <typename T>
concept HasOnHit = requires(T t, PhysicsOnHitData d) { t.OnHit(d); }; ///< Physics contact event.
template <typename T>
concept HasOnOverlapBegin = requires(T t, PhysicsOverlapData d) { t.OnOverlapBegin(d); }; ///< Trigger overlap start.
template <typename T>
concept HasOnOverlapEnd = requires(T t, PhysicsOverlapData d) { t.OnOverlapEnd(d); }; ///< Trigger overlap end.
template <typename T>
concept HasDefineSchema = requires(T t) { t.DefineSchema(); }; ///< Compile-time field schema declaration.
template <typename T>
concept HasDefineFields = requires(T t) { t.DefineFields(); }; ///< Runtime field binding.
/// @}

/// @brief Wide SIMD update function pointer: (dt, fieldArrayTable, FlagBase, entityCount).
using UpdateFunc = void (*)(SimFloat, void**, void*, uint32_t);

/// @brief Single-entity init function pointer: (fieldArrayTable, FlagBase, localIndex).
using InitFunc = void (*)(void**, void*, uint32_t);

/// @brief Per-entity-type metadata stored in @ref ReflectionRegistry.
///
/// Populated at static init by @c PrefabReflector::RegisterPrefab. Function
/// pointers are NOT serializable; they are merged from reflected data by name
/// when a baked snapshot is loaded.
struct EntityMeta
{
	const char* Name = nullptr; ///< Registered entity type name (from TNX_REGISTER_ENTITY).
	size_t ViewSize  = 0;       ///< sizeof the scalar view type.

	UpdateFunc PrePhys      = nullptr; ///< Wide SIMD pre-physics sweep.
	UpdateFunc PostPhys     = nullptr; ///< Wide SIMD post-physics sweep.
	UpdateFunc ScalarUpdate = nullptr; ///< Scalar per-entity update.
	InitFunc Initialize     = nullptr; ///< Single-entity initialization after spawn.

#ifdef TNX_ENABLE_ROLLBACK
	/// Resim variants: scalar loop filtered by @c DirtiedFrame to skip unchanged entities.
	UpdateFunc PrePhysResim  = nullptr;
	UpdateFunc PostPhysResim = nullptr;
#endif

	uint32_t EntitiesPerChunk = 256; ///< Archetype chunk capacity for this entity type.

	EntityMeta() = default;

	EntityMeta(const size_t inViewSize, const UpdateFunc prePhys,
		const UpdateFunc postPhys, const UpdateFunc scalarUpdate)
		: ViewSize(inViewSize),
		  PrePhys(prePhys),
		  PostPhys(postPhys),
		  ScalarUpdate(scalarUpdate)
	{
	}
};
