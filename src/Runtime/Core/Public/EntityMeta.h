#pragma once

#include "Types.h"

// ---------------------------------------------------------------------------
// Concept detection for entity lifecycle hooks.
// Implement the method → get the tick. Don't implement it → pay nothing.
// ---------------------------------------------------------------------------
enum class SystemID : uint8_t;

template <typename T> concept HasOnCreate     = requires(T t) { t.OnCreate(); };
template <typename T> concept HasOnDestroy    = requires(T t) { t.OnDestroy(); };
template <typename T> concept HasScalarUpdate = requires(T t, SimFloat dt) { t.ScalarUpdate(dt); };
template <typename T> concept HasPrePhysics   = requires(T t, SimFloat dt) { t.PrePhysics(dt); };
template <typename T> concept HasPostPhysics  = requires(T t, SimFloat dt) { t.PostPhysics(dt); };
template <typename T> concept HasPhysicsStep  = requires(T t, SimFloat dt) { t.PhysicsStep(dt); };
template <typename T> concept HasOnActivate   = requires(T t) { t.OnActivate(); };
template <typename T> concept HasOnDeactivate = requires(T t) { t.OnDeactivate(); };
template <typename T> concept HasOnCollide    = requires(T t) { t.OnCollide(); };
template <typename T> concept HasDefineSchema = requires(T t) { t.DefineSchema(); };
template <typename T> concept HasDefineFields = requires(T t) { t.DefineFields(); };

// Function pointer type for wide/masked entity update dispatch.
using UpdateFunc = void(*)(SimFloat, void**, void*, uint32_t);

// ---------------------------------------------------------------------------
// EntityMeta — per-entity-type metadata stored in ReflectionRegistry.
//
// Function pointers (PrePhys, PostPhys, ScalarUpdate) are NOT serializable.
// They are populated at static init by PrefabReflector and merged by name
// when loading baked data.
// ---------------------------------------------------------------------------
struct EntityMeta
{
	const char* Name = nullptr;
	size_t ViewSize  = 0;

	UpdateFunc PrePhys      = nullptr;
	UpdateFunc PostPhys     = nullptr;
	UpdateFunc ScalarUpdate = nullptr;

	uint32_t EntitiesPerChunk = 256;

	EntityMeta() = default;

	EntityMeta(const size_t inViewSize, const UpdateFunc prePhys,
			   const UpdateFunc postPhys, const UpdateFunc scalarUpdate)
		: ViewSize(inViewSize)
		, PrePhys(prePhys)
		, PostPhys(postPhys)
		, ScalarUpdate(scalarUpdate)
	{
	}
};
