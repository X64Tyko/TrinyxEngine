#pragma once
#include "FieldProxy.h"

template< template <bool> class Derived, bool MASK = false>
struct ComponentView
{
};

template< template <bool> class CLASS, template <typename, bool> class SUPER = ComponentView, bool MASK = false>
using InheritableComp = SUPER<CLASS<MASK>, MASK>;

template< typename CLASS, template <typename, bool> class SUPER = ComponentView, bool MASK = false>
using FinalComp = SUPER<CLASS, MASK>;

template <bool MASK = false>
using FloatProxy = FieldProxy<float, MASK>;
template <bool MASK = false>
using IntProxy = FieldProxy<int32_t, MASK>;
template <bool MASK = false>
using UIntProxy = FieldProxy<uint32_t, MASK>;
template <bool MASK = false>
using Int64Proxy = FieldProxy<int64_t, MASK>;
template <bool MASK = false>
using UInt64Proxy = FieldProxy<uint64_t, MASK>;
