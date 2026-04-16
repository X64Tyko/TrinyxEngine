#pragma once
#include <cstdint>
#include <type_traits>
#include <immintrin.h>

#include "Logger.h"
#include "SchemaValidation.h"

namespace FieldProxyConsts
{
	static const __m256i element_indices = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7); // Indices of elements
}

template <typename T, typename FIELDTYPE, typename VECTYPE>
concept ProxyType = std::is_same_v<std::remove_cvref_t<T>, FIELDTYPE> || std::is_same_v<std::remove_cvref_t<T>, VECTYPE> || SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value;

// SIMD type traits for selecting correct intrinsics
template <typename T, FieldWidth WIDTH>
struct SIMDTraits;

template <typename FieldType, typename VecType, FieldWidth WIDTH>
struct FieldMask
{
	VecType mask; // Or __m256 for float comparisons
	using Traits = SIMDTraits<FieldType, WIDTH>;

	// The "Choose" method for your syntax
	template <ProxyType<FieldType, VecType> TVAL, ProxyType<FieldType, VecType> FVAL>
	FORCE_INLINE decltype(auto) Choose(TVAL&& TrueVal, FVAL&& FalseVal) const
	{
		VecType falseV;
		if constexpr (std::is_same_v<FVAL, VecType>) falseV = FalseVal;
		if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<FVAL>>::value) falseV = Traits::load(&FalseVal.WriteArray[FalseVal.index]);
		else if constexpr (std::is_same_v<FVAL, std::remove_cvref_t<FieldType>>) falseV = Traits::set1(FalseVal);
		else falseV                                                                     = FalseVal;

		VecType trueV;
		if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<TVAL>>::value) trueV = Traits::load(&TrueVal.WriteArray[TrueVal.index]);
		else if constexpr (std::is_same_v<TVAL, std::remove_cvref_t<FieldType>>) trueV = Traits::set1(TrueVal);
		else trueV                                                                     = TrueVal;

		return _mm256_blendv_ps(falseV, trueV, mask);
	}

	FieldMask& operator&=(const __m256i other)
	{
		mask = other;
		return *this;
	}
};

template <FieldWidth WIDTH>
struct SIMDTraits<float, WIDTH>
{
	using VecType = __m256;
	static FORCE_INLINE VecType load(const float* ptr) { return _mm256_loadu_ps(ptr); }

	static FORCE_INLINE void store(float* ptr, [[maybe_unused]] __m256i mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask)
		{
			// blendv avoids _mm256_maskstore_ps which MSVC can miscompile in load-modify-store
			// sequences with __restrict pointers — treating the masked store as an unmasked one.
			_mm256_storeu_ps(ptr, _mm256_blendv_ps(_mm256_loadu_ps(ptr), val, _mm256_castsi256_ps(mask)));
		}
		else { _mm256_storeu_ps(ptr, val); }
	}

	// Non-temporal store (bypasses cache, for write-only temporal data)
	static FORCE_INLINE void stream(float* ptr, [[maybe_unused]] __m256i mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask)
		{
			_mm256_storeu_ps(ptr, _mm256_blendv_ps(_mm256_loadu_ps(ptr), val, _mm256_castsi256_ps(mask)));
		}
		else { _mm256_stream_ps(ptr, val); }
	}

	static FORCE_INLINE VecType set1(float val) { return _mm256_set1_ps(val); }
	static FORCE_INLINE VecType add(VecType a, VecType b) { return _mm256_add_ps(a, b); }
	static FORCE_INLINE VecType sub(VecType a, VecType b) { return _mm256_sub_ps(a, b); }
	static FORCE_INLINE VecType mul(VecType a, VecType b) { return _mm256_mul_ps(a, b); }
	static FORCE_INLINE VecType div(VecType a, VecType b) { return _mm256_div_ps(a, b); }
	static FORCE_INLINE FieldMask<float, VecType, WIDTH> GT(VecType a, VecType b) { return {_mm256_cmp_ps(a, b, _CMP_GT_OQ)}; }
	static FORCE_INLINE FieldMask<float, VecType, WIDTH> LT(VecType a, VecType b) { return {_mm256_cmp_ps(a, b, _CMP_LT_OQ)}; }
	static FORCE_INLINE FieldMask<float, VecType, WIDTH> GE(VecType a, VecType b) { return {_mm256_cmp_ps(a, b, _CMP_GE_OQ)}; }
	static FORCE_INLINE FieldMask<float, VecType, WIDTH> LE(VecType a, VecType b) { return {_mm256_cmp_ps(a, b, _CMP_LE_OQ)}; }
	static FORCE_INLINE FieldMask<float, VecType, WIDTH> EQ(VecType a, VecType b) { return {_mm256_cmp_ps(a, b, _CMP_EQ_OQ)}; }
	static FORCE_INLINE FieldMask<float, VecType, WIDTH> NEQ(VecType a, VecType b) { return {_mm256_cmp_ps(a, b, _CMP_NEQ_OQ)}; }
	static FORCE_INLINE VecType Blend(VecType a, VecType b) { return _mm256_blendv_ps(a, b, _mm256_cmp_ps(a, b, _CMP_GT_OQ)); }
	static FORCE_INLINE VecType min(VecType a, VecType b) { return _mm256_min_ps(a, b); }
	static FORCE_INLINE VecType max(VecType a, VecType b) { return _mm256_max_ps(a, b); }
	static FORCE_INLINE VecType abs(VecType a) { return _mm256_andnot_ps(_mm256_set1_ps(-0.0f), a); }
};

template <FieldWidth WIDTH>
struct SIMDTraits<int32_t, WIDTH>
{
	using VecType = __m256i;
	static FORCE_INLINE VecType load(const int32_t* ptr) { return _mm256_loadu_si256((const __m256i*)ptr); }

	static FORCE_INLINE void store(int32_t* ptr, [[maybe_unused]] __m256i mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask)
		{
			_mm256_storeu_si256((__m256i*)ptr, _mm256_blendv_epi8(_mm256_loadu_si256((const __m256i*)ptr), val, mask));
		}
		else { _mm256_storeu_si256((__m256i*)ptr, val); }
	}

	// Non-temporal store (bypasses cache, for write-only temporal data)
	static FORCE_INLINE void stream(int32_t* ptr, [[maybe_unused]] __m256i mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask)
		{
			_mm256_storeu_si256((__m256i*)ptr, _mm256_blendv_epi8(_mm256_loadu_si256((const __m256i*)ptr), val, mask));
		}
		else { _mm256_stream_si256((__m256i*)ptr, val); }
	}

	static FORCE_INLINE VecType set1(int32_t val) { return _mm256_set1_epi32(val); }
	static FORCE_INLINE VecType add(VecType a, VecType b) { return _mm256_add_epi32(a, b); }
	static FORCE_INLINE VecType sub(VecType a, VecType b) { return _mm256_sub_epi32(a, b); }
	static FORCE_INLINE VecType mul(VecType a, VecType b) { return _mm256_mullo_epi32(a, b); }

	static FORCE_INLINE VecType div(VecType a, VecType b)
	{
		// Integer division has no SIMD intrinsic - fall back to scalar
		alignas(32) int32_t aData[8], bData[8], result[8];
		_mm256_store_si256((__m256i*)aData, a);
		_mm256_store_si256((__m256i*)bData, b);
		for (int i = 0; i < 8; ++i) result[i] = aData[i] / bData[i];
		return _mm256_load_si256((__m256i*)result);
	}

	static FORCE_INLINE FieldMask<int32_t, VecType, WIDTH> GT(VecType a, VecType b) { return _mm256_cmpgt_epi32(a, b); }
	static FORCE_INLINE FieldMask<int32_t, VecType, WIDTH> LT(VecType a, VecType b) { return _mm256_cmpgt_epi32(b, a); }
	static FORCE_INLINE FieldMask<int32_t, VecType, WIDTH> EQ(VecType a, VecType b) { return {_mm256_cmpeq_epi32(a, b)}; }

	static FORCE_INLINE FieldMask<int32_t, VecType, WIDTH> NEQ(VecType a, VecType b)
	{
		return {_mm256_andnot_si256(_mm256_cmpeq_epi32(a, b), _mm256_set1_epi32(-1))};
	}

	static FORCE_INLINE VecType min(VecType a, VecType b) { return _mm256_min_epi32(a, b); }
	static FORCE_INLINE VecType max(VecType a, VecType b) { return _mm256_max_epi32(a, b); }
	static FORCE_INLINE VecType abs(VecType a) { return _mm256_abs_epi32(a); }
};

template <FieldWidth WIDTH>
struct SIMDTraits<uint32_t, WIDTH>
{
	using VecType = __m256i;
	static FORCE_INLINE VecType load(const uint32_t* ptr) { return _mm256_loadu_si256((const __m256i*)ptr); }

	static FORCE_INLINE void store(uint32_t* ptr, [[maybe_unused]] __m256i mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask)
		{
			_mm256_storeu_si256((__m256i*)ptr, _mm256_blendv_epi8(_mm256_loadu_si256((const __m256i*)ptr), val, mask));
		}
		else { _mm256_storeu_si256((__m256i*)ptr, val); }
	}

	// Non-temporal store (bypasses cache, for write-only temporal data)
	static FORCE_INLINE void stream(uint32_t* ptr, [[maybe_unused]] __m256i mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask)
		{
			_mm256_storeu_si256((__m256i*)ptr, _mm256_blendv_epi8(_mm256_loadu_si256((const __m256i*)ptr), val, mask));
		}
		else { _mm256_stream_si256((__m256i*)ptr, val); }
	}

	static FORCE_INLINE VecType set1(uint32_t val) { return _mm256_set1_epi32(val); }
	static FORCE_INLINE VecType add(VecType a, VecType b) { return _mm256_add_epi32(a, b); }
	static FORCE_INLINE VecType sub(VecType a, VecType b) { return _mm256_sub_epi32(a, b); }
	static FORCE_INLINE VecType mul(VecType a, VecType b) { return _mm256_mullo_epi32(a, b); }

	static FORCE_INLINE VecType div(VecType a, VecType b)
	{
		alignas(32) uint32_t aData[8], bData[8], result[8];
		_mm256_store_si256((__m256i*)aData, a);
		_mm256_store_si256((__m256i*)bData, b);
		for (int i = 0; i < 8; ++i) result[i] = aData[i] / bData[i];
		return _mm256_load_si256((__m256i*)result);
	}

	static FORCE_INLINE FieldMask<uint32_t, VecType, WIDTH> GT(VecType a, VecType b) { return _mm256_cmpgt_epi32(a, b); }
	static FORCE_INLINE FieldMask<uint32_t, VecType, WIDTH> LT(VecType a, VecType b) { return _mm256_cmpgt_epi32(b, a); }
	static FORCE_INLINE FieldMask<uint32_t, VecType, WIDTH> EQ(VecType a, VecType b) { return {_mm256_cmpeq_epi32(a, b)}; }

	static FORCE_INLINE FieldMask<uint32_t, VecType, WIDTH> NEQ(VecType a, VecType b)
	{
		return {_mm256_andnot_si256(_mm256_cmpeq_epi32(a, b), _mm256_set1_epi32(-1))};
	}

	static FORCE_INLINE VecType min(VecType a, VecType b) { return _mm256_min_epu32(a, b); }
	static FORCE_INLINE VecType max(VecType a, VecType b) { return _mm256_max_epu32(a, b); }
};

// Conditional mask storage: Wide/WideMask need a 32-byte __m256i mask; Scalar does not.
// Storing it unconditionally wastes 32 bytes per FieldProxy in Scalar mode.
// With e.g. 9 fields in Transform that is 288 bytes of dead weight per entity view.
template <FieldWidth WIDTH>
struct FieldProxyMask
{
	__m256i mask = _mm256_set1_epi64x(-1);
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
			const __m256i count_vec = _mm256_set1_epi32(startCount);
			this->mask              = _mm256_cmpgt_epi32(count_vec, FieldProxyConsts::element_indices);
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
			__m256i flags = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&FlagsArray[index]));
			flags         = _mm256_or_si256(flags, _mm256_set1_epi32(DirtyMask));
			_mm256_storeu_si256(reinterpret_cast<__m256i*>(&FlagsArray[index]), flags);
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
