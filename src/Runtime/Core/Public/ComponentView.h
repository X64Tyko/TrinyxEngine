#pragma once
#include "FieldProxy.h"

template< template <FieldWidth> class Derived, FieldWidth WIDTH = FieldWidth::Scalar>
struct ComponentView
{
};

template< template <FieldWidth> class CLASS, template <typename, FieldWidth> class SUPER = ComponentView, FieldWidth WIDTH = FieldWidth::Scalar>
using InheritableComp = SUPER<CLASS<WIDTH>, WIDTH>;

template< typename CLASS, template <typename, FieldWidth> class SUPER = ComponentView, FieldWidth WIDTH = FieldWidth::Scalar>
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
