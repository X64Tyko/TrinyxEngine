#pragma once
#include "FieldProxy.h"

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