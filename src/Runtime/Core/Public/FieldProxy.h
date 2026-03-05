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
		if constexpr (WIDTH == FieldWidth::WideMask) { _mm256_maskstore_ps(ptr, mask, val); }
		else { _mm256_storeu_ps(ptr, val); }
	}

	// Non-temporal store (bypasses cache, for write-only temporal data)
	static FORCE_INLINE void stream(float* ptr, [[maybe_unused]] __m256i mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask) { _mm256_maskstore_ps(ptr, mask, val); } // No masked stream, fall back
		else { _mm256_stream_ps(ptr, val); }
	}

	static FORCE_INLINE VecType set1(float val) { return _mm256_set1_ps(val); }
	static FORCE_INLINE VecType add(VecType a, VecType b) { return _mm256_add_ps(a, b); }
	static FORCE_INLINE VecType sub(VecType a, VecType b) { return _mm256_sub_ps(a, b); }
	static FORCE_INLINE VecType mul(VecType a, VecType b) { return _mm256_mul_ps(a, b); }
	static FORCE_INLINE VecType div(VecType a, VecType b) { return _mm256_div_ps(a, b); }
	static FORCE_INLINE FieldMask<float, VecType, WIDTH> GT(VecType a, VecType b) { return {_mm256_cmp_ps(a, b, _CMP_GT_OQ)}; }
	static FORCE_INLINE FieldMask<float, VecType, WIDTH> LT(VecType a, VecType b) { return {_mm256_cmp_ps(a, b, _CMP_LT_OQ)}; }
	static FORCE_INLINE VecType Blend(VecType a, VecType b) { return _mm256_blendv_ps(a, b, _mm256_cmp_ps(a, b, _CMP_GT_OQ)); }
};

template <FieldWidth WIDTH>
struct SIMDTraits<int32_t, WIDTH>
{
	using VecType = __m256i;
	static FORCE_INLINE VecType load(const int32_t* ptr) { return _mm256_loadu_si256((const __m256i*)ptr); }

	static FORCE_INLINE void store(int32_t* ptr, [[maybe_unused]] __m256i mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask) { _mm256_maskstore_epi32(ptr, mask, val); }
		else { _mm256_storeu_si256((__m256i*)ptr, val); }
	}

	// Non-temporal store (bypasses cache, for write-only temporal data)
	static FORCE_INLINE void stream(int32_t* ptr, [[maybe_unused]] __m256i mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask) { _mm256_maskstore_epi32(ptr, mask, val); } // No masked stream, fall back
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
};

template <FieldWidth WIDTH>
struct SIMDTraits<uint32_t, WIDTH>
{
	using VecType = __m256i;
	static FORCE_INLINE VecType load(const uint32_t* ptr) { return _mm256_loadu_si256((const __m256i*)ptr); }

	static FORCE_INLINE void store(uint32_t* ptr, [[maybe_unused]] __m256i mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask) { _mm256_maskstore_epi32((int32_t*)ptr, mask, val); }
		else { _mm256_storeu_si256((__m256i*)ptr, val); }
	}

	// Non-temporal store (bypasses cache, for write-only temporal data)
	static FORCE_INLINE void stream(uint32_t* ptr, [[maybe_unused]] __m256i mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask) { _mm256_maskstore_epi32((int32_t*)ptr, mask, val); } // No masked stream, fall back
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

	FieldType* __restrict WriteArray = nullptr; // Current frame — pre-frame memcpy seeds old state before any updates run
	uint32_t index;

	explicit operator typename Traits::VecType() const
	{
		if constexpr (WIDTH == FieldWidth::Scalar) return WriteArray[index];
		else return Traits::load(&WriteArray[index]);
	}

	template <ProxyType<FieldType, typename Traits::VecType> T>
	FORCE_INLINE FieldMask<FieldType, typename Traits::VecType, WIDTH> operator>(T threshold) const
	{
		typename Traits::VecType cmp;
		if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value) cmp = Traits::load(&threshold.WriteArray[threshold.index]);
		else if constexpr (std::is_same_v<T, std::remove_cvref_t<FieldType>>) cmp = Traits::set1(threshold);
		else cmp                                                                  = threshold;

		// Compare current SIMD lane (this->data) with threshold
		return Traits::GT(Traits::load(&WriteArray[index]), cmp);
	}

	template <ProxyType<FieldType, typename Traits::VecType> T>
	FORCE_INLINE FieldMask<FieldType, typename Traits::VecType, WIDTH> operator<(T threshold) const
	{
		typename Traits::VecType cmp;
		if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value) cmp = Traits::load(&threshold.WriteArray[threshold.index]);
		else if constexpr (std::is_same_v<T, std::remove_cvref_t<FieldType>>) cmp = Traits::set1(threshold);
		else cmp                                                                  = threshold;

		// Compare current SIMD lane (this->data) with threshold
		return Traits::LT(Traits::load(&WriteArray[index]), cmp);
	}

	template <ProxyType<FieldType, typename Traits::VecType> T>
	FORCE_INLINE decltype(auto) operator=(volatile T&& value)
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
		return *this;
	}

	// Bind: point at the write frame (pre-frame memcpy already seeded old state into it)
	FORCE_INLINE void Bind(void* writeArray, uint32_t startIndex = 0, int32_t startCount = -1)
	{
		WriteArray = (FieldType*)writeArray;
		index      = startIndex;

		if constexpr (WIDTH != FieldWidth::Scalar)
		{
			const __m256i count_vec = _mm256_set1_epi32(startCount);
			this->mask              = _mm256_cmpgt_epi32(count_vec, FieldProxyConsts::element_indices);
		}
	}

	// Advance: move index forward — no copy needed, pre-frame memcpy already propagated old state
	FORCE_INLINE void Advance(uint32_t step)
	{
		index += step;
	}

	// FRIEND OPERATORS
	template <ProxyType<FieldType, typename Traits::VecType> L, ProxyType<FieldType, typename Traits::VecType> R>
	FORCE_INLINE friend decltype(auto) operator*(L&& LHS, R&& RHS)
	{
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			FieldType LVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<L>>::value) LVal = LHS.WriteArray[LHS.index];
			else if constexpr (std::is_same_v<L, std::remove_cvref_t<FieldType>>) LVal = LHS;

			FieldType RVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<R>>::value) RVal = RHS.WriteArray[RHS.index];
			else if constexpr (std::is_same_v<R, std::remove_cvref_t<FieldType>>) RVal = RHS;

			return LVal * RVal;
		}
		else
		{
			typename Traits::VecType LVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<L>>::value) LVal = Traits::load(&LHS.WriteArray[LHS.index]);
			else if constexpr (std::is_same_v<L, std::remove_cvref_t<FieldType>>) LVal = Traits::set1(LHS);
			else LVal                                                                  = LHS;

			typename Traits::VecType RVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<R>>::value) RVal = Traits::load(&RHS.WriteArray[RHS.index]);
			else if constexpr (std::is_same_v<R, std::remove_cvref_t<FieldType>>) RVal = Traits::set1(RHS);
			else RVal                                                                  = RHS;

			return Traits::mul(LVal, RVal);
		}
	}

	// FRIEND OPERATORS
	template <ProxyType<FieldType, typename Traits::VecType> L, ProxyType<FieldType, typename Traits::VecType> R>
	FORCE_INLINE friend decltype(auto) operator+(L&& LHS, R&& RHS)
	{
		if constexpr (WIDTH == FieldWidth::Scalar)
		{
			FieldType LVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<L>>::value) LVal = LHS.WriteArray[LHS.index];
			else if constexpr (std::is_same_v<L, std::remove_cvref_t<FieldType>>) LVal = LHS;

			FieldType RVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<R>>::value) RVal = RHS.WriteArray[RHS.index];
			else if constexpr (std::is_same_v<R, std::remove_cvref_t<FieldType>>) RVal = RHS;

			return LVal + RVal;
		}
		else
		{
			typename Traits::VecType LVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<L>>::value) LVal = Traits::load(&LHS.WriteArray[LHS.index]);
			else if constexpr (std::is_same_v<L, std::remove_cvref_t<FieldType>>) LVal = Traits::set1(LHS);
			else LVal                                                                  = LHS;

			typename Traits::VecType RVal;
			if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<R>>::value) RVal = Traits::load(&RHS.WriteArray[RHS.index]);
			else if constexpr (std::is_same_v<R, std::remove_cvref_t<FieldType>>) RVal = Traits::set1(RHS);
			else RVal                                                                  = RHS;

			return Traits::add(LVal, RVal);
		}
	}
};