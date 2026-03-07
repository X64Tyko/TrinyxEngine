#pragma once
#include "FieldProxy.h"

// Identifies which engine systems access a component's data each frame.
//   Physics only          → Phys partition   (Arena 1, grows right from 0)
//   Render only           → Render partition  (Arena 2, grows right from MAX_PHYSICS)
//   Physics | Render      → Dual partition    (Arena 1, grows left from MAX_PHYSICS)
//   Logic | None          → Logic partition   (Arena 2, grows left from MAX_CACHED)
enum class SystemID : uint8_t
{
	None    = 0,      // Partition-agnostic — doesn't influence group derivation (e.g. Transform)
	Physics = 1 << 0, // Physics solver reads/writes this component (e.g. RigidBody, Forces)
	Render  = 1 << 1, // Render thread reads this component for GPU upload (e.g. ColorData, MeshRef)
	Logic   = 1 << 2, // Logic-only access, no physics or render (e.g. Stats, AI state)

	Dual = Physics | Render, // Convenience: entity participates in both physics and render passes
	All  = Physics | Render | Logic,
};

template <template <FieldWidth> class Derived, FieldWidth WIDTH = FieldWidth::Scalar>
struct ComponentView
{
	// Called at registration time to get (and cache) the unique slab slot for this component.
	// Safe to call multiple times — counter is incremented exactly once per (Derived, WIDTH) pair.
	static uint8_t StaticTemporalIndex()
	{
		static uint8_t idx = Derived<WIDTH>::GetTemporalIndex();
		return idx;
	}

	static ComponentTypeID StaticTypeID()
	{
		static ComponentTypeID id = Internal::g_GlobalComponentCounter++;
		return id;
	}

protected:
	static uint8_t GetTemporalIndex()
	{
		if constexpr (requires { Derived<WIDTH>::TemporalTier; })
		{
#ifndef TNX_ENABLE_ROLLBACK
			if (Derived<WIDTH>::TemporalTier == CacheTier::Temporal) return Internal::g_TemporalComponentCounter[static_cast<size_t>(CacheTier::Volatile)]++;
#endif
			return Internal::g_TemporalComponentCounter[static_cast<size_t>(Derived<WIDTH>::TemporalTier)]++;
		}
		return 0xFF;
	}
};

template <template <FieldWidth> class CLASS, template <typename, FieldWidth> class SUPER = ComponentView, FieldWidth WIDTH = FieldWidth::Scalar>
using InheritableComp = SUPER<CLASS<WIDTH>, WIDTH>;

template <typename CLASS, template <typename, FieldWidth> class SUPER = ComponentView, FieldWidth WIDTH = FieldWidth::Scalar>
using FinalComp = SUPER<CLASS, WIDTH>;

template <FieldWidth WIDTH = FieldWidth::Scalar>
using FloatProxy = FieldProxy<float, WIDTH>;
template <FieldWidth WIDTH = FieldWidth::Scalar>
using IntProxy = FieldProxy<int32_t, WIDTH>;
template <FieldWidth WIDTH = FieldWidth::Scalar>
using UIntProxy = FieldProxy<uint32_t, WIDTH>;
template <FieldWidth WIDTH = FieldWidth::Scalar>
using Int64Proxy = FieldProxy<int64_t, WIDTH>;
template <FieldWidth WIDTH = FieldWidth::Scalar>
using UInt64Proxy = FieldProxy<uint64_t, WIDTH>;


// Overload the bitwise AND operator
inline SystemID operator&(SystemID lhs, SystemID rhs)
{
	return static_cast<SystemID>(
		static_cast<std::underlying_type_t<SystemID>>(lhs) &
		static_cast<std::underlying_type_t<SystemID>>(rhs)
	);
}

// Overload the bitwise OR operator
inline SystemID operator|(SystemID lhs, SystemID rhs)
{
	return static_cast<SystemID>(
		static_cast<std::underlying_type_t<SystemID>>(lhs) |
		static_cast<std::underlying_type_t<SystemID>>(rhs)
	);
}

// Overload the bitwise OR operator
inline SystemID operator|=(SystemID lhs, SystemID rhs)
{
	return static_cast<SystemID>(
		static_cast<std::underlying_type_t<SystemID>>(lhs) |
		static_cast<std::underlying_type_t<SystemID>>(rhs)
	);
}

inline bool Equal(SystemID lhs, SystemID rhs)
{
	return static_cast<SystemID>(static_cast<std::underlying_type_t<SystemID>>(lhs) & static_cast<std::underlying_type_t<SystemID>>(rhs)) == rhs;
}