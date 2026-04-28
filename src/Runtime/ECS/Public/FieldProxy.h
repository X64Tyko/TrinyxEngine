#pragma once
#include <cstdint>
#include <type_traits>
#include <vector>

#include "Logger.h"
#include "SchemaValidation.h"
#include "SIMDTraits.h" // SIMDTraits, FieldMask, kSIMDWide32Lanes

template <typename T, typename FIELDTYPE, typename VECTYPE>
concept ProxyType = std::is_same_v<std::remove_cvref_t<T>, FIELDTYPE> || std::is_same_v<std::remove_cvref_t<T>, VECTYPE> || SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value;

// Conditional mask storage: Wide/WideMask need a 32-byte __m256i mask; Scalar does not.
// Storing it unconditionally wastes 32 bytes per FieldProxy in Scalar mode.
// With e.g. 9 fields in Transform that is 288 bytes of dead weight per entity view.
template <FieldWidth WIDTH>
struct FieldProxyMask
{
	__m256i mask = _mm256_set1_epi64x(-1); // all lanes set — default is "store everything"
};

template <>
struct FieldProxyMask<FieldWidth::Scalar>
{
}; // Zero-size base for Scalar

// Helper: Proxy for individual field access with SIMD
template <typename FieldType, FieldWidth WIDTH>
struct FieldProxy : private FieldProxyMask<WIDTH>
{
	using Traits  = SIMDTraits<FieldType, WIDTH>;
	using VecMask = FieldMask<FieldType, typename Traits::VecType, WIDTH>;

	FieldType* __restrict WriteArray = nullptr;
	int32_t* __restrict FlagsArray   = nullptr; // CacheSlotMeta::Flags — dirty bits at 30 (accumulate) and 29 (per-frame)
	uint32_t index;

	static constexpr int32_t DirtyBit        = static_cast<int32_t>(1u << 30);
	static constexpr int32_t DirtiedFrameBit = static_cast<int32_t>(1u << 29);
	static constexpr int32_t DirtyMask       = DirtyBit | DirtiedFrameBit;

	explicit operator typename Traits::VecType() const
	{
		if constexpr (WIDTH == FieldWidth::Scalar) return WriteArray[index];
		else return Traits::load(&WriteArray[index]);
	}

	// Scalar read accessor — returns a copy of the underlying value.
	// Only available in Scalar width (compile error otherwise).
	FieldType Value() const requires (WIDTH == FieldWidth::Scalar) { return WriteArray[index]; }

	// Bind: point at the write frame (pre-frame memcpy already seeded old state into it).
	// flagsArray is the CacheSlotMeta::Flags int32_t* from fieldArrayTable[0].
	FORCE_INLINE void Bind(void* writeArray, void* flagsArray, uint32_t startIndex = 0, int32_t startCount = -1)
	{
		WriteArray = (FieldType*)writeArray;
		FlagsArray = (int32_t*)flagsArray;
		index      = startIndex;

		if constexpr (WIDTH != FieldWidth::Scalar)
		{
			this->mask = Traits::GenerateCountMask(startCount);
		}
	}

	// ── Comparison operators (read-only — do NOT mark dirty) ──

	template <ProxyType<FieldType, typename Traits::VecType> T>
	FORCE_INLINE VecMask operator>(T threshold) const
	{
		typename Traits::VecType cmp;
		if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value) cmp = Traits::load(&threshold.WriteArray[threshold.index]);
		else if constexpr (std::is_same_v<std::remove_cvref_t<T>, std::remove_cvref_t<FieldType>>) cmp = Traits::set1(threshold);
		else cmp                                                                                       = threshold;
		return Traits::GT(Traits::load(&WriteArray[index]), cmp);
	}

	template <ProxyType<FieldType, typename Traits::VecType> T>
	FORCE_INLINE VecMask operator<(T threshold) const
	{
		typename Traits::VecType cmp;
		if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value) cmp = Traits::load(&threshold.WriteArray[threshold.index]);
		else if constexpr (std::is_same_v<std::remove_cvref_t<T>, std::remove_cvref_t<FieldType>>) cmp = Traits::set1(threshold);
		else cmp                                                                                       = threshold;
		return Traits::LT(Traits::load(&WriteArray[index]), cmp);
	}

	template <ProxyType<FieldType, typename Traits::VecType> T>
	FORCE_INLINE VecMask operator>=(T threshold) const
	{
		typename Traits::VecType cmp;
		if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value) cmp = Traits::load(&threshold.WriteArray[threshold.index]);
		else if constexpr (std::is_same_v<std::remove_cvref_t<T>, std::remove_cvref_t<FieldType>>) cmp = Traits::set1(threshold);
		else cmp                                                                                       = threshold;
		return Traits::GE(Traits::load(&WriteArray[index]), cmp);
	}

	template <ProxyType<FieldType, typename Traits::VecType> T>
	FORCE_INLINE VecMask operator<=(T threshold) const
	{
		typename Traits::VecType cmp;
		if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value) cmp = Traits::load(&threshold.WriteArray[threshold.index]);
		else if constexpr (std::is_same_v<std::remove_cvref_t<T>, std::remove_cvref_t<FieldType>>) cmp = Traits::set1(threshold);
		else cmp                                                                                       = threshold;
		return Traits::LE(Traits::load(&WriteArray[index]), cmp);
	}

	template <ProxyType<FieldType, typename Traits::VecType> T>
	FORCE_INLINE VecMask operator==(T threshold) const
	{
		typename Traits::VecType cmp;
		if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value) cmp = Traits::load(&threshold.WriteArray[threshold.index]);
		else if constexpr (std::is_same_v<std::remove_cvref_t<T>, std::remove_cvref_t<FieldType>>) cmp = Traits::set1(threshold);
		else cmp                                                                                       = threshold;
		return Traits::EQ(Traits::load(&WriteArray[index]), cmp);
	}

	template <ProxyType<FieldType, typename Traits::VecType> T>
	FORCE_INLINE VecMask operator!=(T threshold) const
	{
		typename Traits::VecType cmp;
		if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value) cmp = Traits::load(&threshold.WriteArray[threshold.index]);
		else if constexpr (std::is_same_v<std::remove_cvref_t<T>, std::remove_cvref_t<FieldType>>) cmp = Traits::set1(threshold);
		else cmp                                                                                       = threshold;
		return Traits::NEQ(Traits::load(&WriteArray[index]), cmp);
	}

	template <ProxyType<FieldType, typename Traits::VecType> T>
	FORCE_INLINE decltype(auto) operator=(T&& value)
	{
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			WriteArray[index] = value;
		}
		else
		{
			typename Traits::VecType VecVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value) VecVal = Traits::load(&value.WriteArray[value.index]);
			else if constexpr (std::is_same_v<T, std::remove_cvref_t<FieldType>>) VecVal = Traits::set1(value);
			else VecVal                                                                  = value;

			Traits::store(&WriteArray[index], this->mask, VecVal);
		}

		MarkDirty();
		return *this;
	}

	template <ProxyType<FieldType, typename Traits::VecType> T>
	FORCE_INLINE decltype(auto) operator+=(T&& value)
	{
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			WriteArray[index] += value;
		}
		else
		{
			typename Traits::VecType VecVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value) VecVal = Traits::load(&value.WriteArray[value.index]);
			else if constexpr (std::is_same_v<T, std::remove_cvref_t<FieldType>>) VecVal = Traits::set1(value);
			else VecVal                                                                  = value;

			Traits::store(&WriteArray[index], this->mask, Traits::add(Traits::load(&WriteArray[index]), VecVal));
		}

		MarkDirty();
		return *this;
	}

	template <ProxyType<FieldType, typename Traits::VecType> T>
	FORCE_INLINE decltype(auto) operator-=(T&& value)
	{
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			WriteArray[index] -= value;
		}
		else
		{
			typename Traits::VecType VecVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value) VecVal = Traits::load(&value.WriteArray[value.index]);
			else if constexpr (std::is_same_v<T, std::remove_cvref_t<FieldType>>) VecVal = Traits::set1(value);
			else VecVal                                                                  = value;

			Traits::store(&WriteArray[index], this->mask, Traits::sub(Traits::load(&WriteArray[index]), VecVal));
		}

		MarkDirty();
		return *this;
	}

	template <ProxyType<FieldType, typename Traits::VecType> T>
	FORCE_INLINE decltype(auto) operator*=(T&& value)
	{
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			WriteArray[index] *= value;
		}
		else
		{
			typename Traits::VecType VecVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value) VecVal = Traits::load(&value.WriteArray[value.index]);
			else if constexpr (std::is_same_v<T, std::remove_cvref_t<FieldType>>) VecVal = Traits::set1(value);
			else VecVal                                                                  = value;

			Traits::store(&WriteArray[index], this->mask, Traits::mul(Traits::load(&WriteArray[index]), VecVal));
		}

		MarkDirty();
		return *this;
	}

	template <ProxyType<FieldType, typename Traits::VecType> T>
	FORCE_INLINE decltype(auto) operator/=(T&& value)
	{
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			WriteArray[index] /= value;
		}
		else
		{
			typename Traits::VecType VecVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value) VecVal = Traits::load(&value.WriteArray[value.index]);
			else if constexpr (std::is_same_v<T, std::remove_cvref_t<FieldType>>) VecVal = Traits::set1(value);
			else VecVal                                                                  = value;

			Traits::store(&WriteArray[index], this->mask, Traits::div(Traits::load(&WriteArray[index]), VecVal));
		}

		MarkDirty();
		return *this;
	}

	// Mark the current entity/entities as dirty via CacheSlotMeta::Flags.
	// Sets bit 30 (accumulates until render clears) and bit 29 (cleared at frame start).
	// FlagsArray must be non-null for fields that need dirty tracking.
	// CacheSlotMeta::Flags passes nullptr (self-referential — it IS the flags).
	FORCE_INLINE void MarkDirty()
	{
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			FlagsArray[index] |= DirtyMask;
		}
		else
		{
			Traits::StoreFlagsOr(&FlagsArray[index], DirtyMask);
		}
	}

	// Advance: move index forward — no copy needed, pre-frame memcpy already propagated old state
	FORCE_INLINE void Advance(uint32_t step)
	{
		index += step;
	}

	// ── Unary negation ──
	FORCE_INLINE decltype(auto) operator-() const
	{
		if constexpr (WIDTH == FieldWidth::Scalar)
			return -WriteArray[index];
		else
			return Traits::sub(Traits::set1(FieldType(0)), Traits::load(&WriteArray[index]));
	}

	// ── Binary friend operators (return value, no dirty marking) ──
	//
	// GCC 13 bug workaround: friend function templates defined inside class templates are not
	// differentiated by GCC 13 when constraints reference outer-class dependent types, even when
	// those types differ across instantiations. Adding a non-deduced tag parameter of type
	// FieldProxy* (which is FieldProxy<FieldType,WIDTH>* via the injected-class-name) gives each
	// instantiation a syntactically distinct first parameter type, forcing GCC 13 to see them as
	// separate templates. Tag_ always defaults to nullptr; callers use normal infix syntax (a * b).

	template <FieldProxy* Tag_ = nullptr, ProxyType<FieldType, typename Traits::VecType> L, ProxyType<FieldType, typename Traits::VecType> R>
	FORCE_INLINE friend decltype(auto) operator*(L&& LHS, R&& RHS)
	{
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			FieldType LVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<L>>::value) LVal = LHS.WriteArray[LHS.index];
			else if constexpr (std::is_same_v<std::remove_cvref_t<L>, std::remove_cvref_t<FieldType>>) LVal = LHS;

			FieldType RVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<R>>::value) RVal = RHS.WriteArray[RHS.index];
			else if constexpr (std::is_same_v<std::remove_cvref_t<R>, std::remove_cvref_t<FieldType>>) RVal = RHS;

			return LVal * RVal;
		}
		else
		{
			typename Traits::VecType LVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<L>>::value) LVal = Traits::load(&LHS.WriteArray[LHS.index]);
			else if constexpr (std::is_same_v<std::remove_cvref_t<L>, std::remove_cvref_t<FieldType>>) LVal = Traits::set1(LHS);
			else LVal                                                                  = LHS;

			typename Traits::VecType RVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<R>>::value) RVal = Traits::load(&RHS.WriteArray[RHS.index]);
			else if constexpr (std::is_same_v<std::remove_cvref_t<R>, std::remove_cvref_t<FieldType>>) RVal = Traits::set1(RHS);
			else RVal                                                                                       = RHS;

			return Traits::mul(LVal, RVal);
		}
	}

	template <FieldProxy* Tag_ = nullptr, ProxyType<FieldType, typename Traits::VecType> L, ProxyType<FieldType, typename Traits::VecType> R>
	FORCE_INLINE friend decltype(auto) operator+(L&& LHS, R&& RHS)
	{
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			FieldType LVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<L>>::value) LVal = LHS.WriteArray[LHS.index];
			else if constexpr (std::is_same_v<std::remove_cvref_t<L>, std::remove_cvref_t<FieldType>>) LVal = LHS;

			FieldType RVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<R>>::value) RVal = RHS.WriteArray[RHS.index];
			else if constexpr (std::is_same_v<std::remove_cvref_t<R>, std::remove_cvref_t<FieldType>>) RVal = RHS;

			return LVal + RVal;
		}
		else
		{
			typename Traits::VecType LVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<L>>::value) LVal = Traits::load(&LHS.WriteArray[LHS.index]);
			else if constexpr (std::is_same_v<std::remove_cvref_t<L>, std::remove_cvref_t<FieldType>>) LVal = Traits::set1(LHS);
			else LVal                                                                                       = LHS;

			typename Traits::VecType RVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<R>>::value) RVal = Traits::load(&RHS.WriteArray[RHS.index]);
			else if constexpr (std::is_same_v<std::remove_cvref_t<R>, std::remove_cvref_t<FieldType>>) RVal = Traits::set1(RHS);
			else RVal                                                                                       = RHS;

			return Traits::add(LVal, RVal);
		}
	}

	template <FieldProxy* Tag_ = nullptr, ProxyType<FieldType, typename Traits::VecType> L, ProxyType<FieldType, typename Traits::VecType> R>
	FORCE_INLINE friend decltype(auto) operator-(L&& LHS, R&& RHS)
	{
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			FieldType LVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<L>>::value) LVal = LHS.WriteArray[LHS.index];
			else if constexpr (std::is_same_v<std::remove_cvref_t<L>, std::remove_cvref_t<FieldType>>) LVal = LHS;

			FieldType RVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<R>>::value) RVal = RHS.WriteArray[RHS.index];
			else if constexpr (std::is_same_v<std::remove_cvref_t<R>, std::remove_cvref_t<FieldType>>) RVal = RHS;

			return LVal - RVal;
		}
		else
		{
			typename Traits::VecType LVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<L>>::value) LVal = Traits::load(&LHS.WriteArray[LHS.index]);
			else if constexpr (std::is_same_v<std::remove_cvref_t<L>, std::remove_cvref_t<FieldType>>) LVal = Traits::set1(LHS);
			else LVal                                                                                       = LHS;

			typename Traits::VecType RVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<R>>::value) RVal = Traits::load(&RHS.WriteArray[RHS.index]);
			else if constexpr (std::is_same_v<std::remove_cvref_t<R>, std::remove_cvref_t<FieldType>>) RVal = Traits::set1(RHS);
			else RVal                                                                                       = RHS;

			return Traits::sub(LVal, RVal);
		}
	}

	template <FieldProxy* Tag_ = nullptr, ProxyType<FieldType, typename Traits::VecType> L, ProxyType<FieldType, typename Traits::VecType> R>
	FORCE_INLINE friend decltype(auto) operator/(L&& LHS, R&& RHS)
	{
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			FieldType LVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<L>>::value) LVal = LHS.WriteArray[LHS.index];
			else if constexpr (std::is_same_v<std::remove_cvref_t<L>, std::remove_cvref_t<FieldType>>) LVal = LHS;

			FieldType RVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<R>>::value) RVal = RHS.WriteArray[RHS.index];
			else if constexpr (std::is_same_v<std::remove_cvref_t<R>, std::remove_cvref_t<FieldType>>) RVal = RHS;

			return LVal / RVal;
		}
		else
		{
			typename Traits::VecType LVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<L>>::value) LVal = Traits::load(&LHS.WriteArray[LHS.index]);
			else if constexpr (std::is_same_v<std::remove_cvref_t<L>, std::remove_cvref_t<FieldType>>) LVal = Traits::set1(LHS);
			else LVal                                                                  = LHS;

			typename Traits::VecType RVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<R>>::value) RVal = Traits::load(&RHS.WriteArray[RHS.index]);
			else if constexpr (std::is_same_v<std::remove_cvref_t<R>, std::remove_cvref_t<FieldType>>) RVal = Traits::set1(RHS);
			else RVal                                                                  = RHS;

			return Traits::div(LVal, RVal);
		}
	}
};
