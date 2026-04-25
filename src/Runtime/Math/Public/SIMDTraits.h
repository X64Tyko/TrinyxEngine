#pragma once

#include <cstdint>
#include <type_traits>

#include "Types.h"             // FieldWidth, FORCE_INLINE
#include "Fixed32.h"           // Fixed32
#include "SchemaValidation.h"  // IsFieldProxy — used by FieldMask::Choose

// ---------------------------------------------------------------------------
// SIMDTraits — per-(type, width, ISA) primitives for FieldProxy and friends.
//
// The active SIMD backend is picked at compile time:
//   - TNX_SIMD_AVX512  — 16 lanes / 32-bit element. (Stub — not yet implemented.)
//   - TNX_SIMD_AVX2    — 8  lanes / 32-bit element. (Current shipping path.)
//   - TNX_SIMD_NEON    — 4  lanes / 32-bit element. (Stub — not yet implemented.)
//   - TNX_SIMD_SCALAR_ONLY — Wide unavailable; only FieldWidth::Scalar compiles.
//
// One backend macro is defined per build. Force a specific one with:
//   -DTNX_SIMD_FORCE_AVX2 / TNX_SIMD_FORCE_NEON / TNX_SIMD_FORCE_SCALAR
//
// Ability detection:
//   The primary `SIMDTraits<T, WIDTH>` template static_asserts when WIDTH != Scalar
//   and T has no specialization on the active ISA.
// ---------------------------------------------------------------------------

// --- ISA selection ----------------------------------------------------------

#if defined(TNX_SIMD_FORCE_SCALAR)
#define TNX_SIMD_SCALAR_ONLY 1
#elif defined(TNX_SIMD_FORCE_NEON)
#define TNX_SIMD_NEON 1
#elif defined(TNX_SIMD_FORCE_AVX2)
#define TNX_SIMD_AVX2 1
#elif defined(__AVX512F__)
#define TNX_SIMD_AVX512 1
#elif defined(__AVX2__)
#define TNX_SIMD_AVX2 1
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
#define TNX_SIMD_NEON 1
#else
#define TNX_SIMD_SCALAR_ONLY 1
#endif

#if defined(TNX_SIMD_AVX512) || defined(TNX_SIMD_AVX2)
#include <immintrin.h>
#elif defined(TNX_SIMD_NEON)
#include <arm_neon.h>
#endif

// --- Lane count for 32-bit elements ----------------------------------------

#if defined(TNX_SIMD_AVX512)
inline constexpr int kSIMDWide32Lanes = 16;
#elif defined(TNX_SIMD_AVX2)
inline constexpr int kSIMDWide32Lanes = 8;
#elif defined(TNX_SIMD_NEON)
inline constexpr int kSIMDWide32Lanes = 4;
#else
inline constexpr int kSIMDWide32Lanes = 1;
#endif

// --- FieldMask forward decl (defined per ISA below) ------------------------

template <typename FieldType, typename VecType, FieldWidth WIDTH>
struct FieldMask;

// --- Primary template — ability detection ----------------------------------
// Triggers a clear static_assert when a Wide field is requested for a type that
// has no specialization on the active ISA. Scalar paths still compile because
// FieldProxy never touches Traits::VecType in scalar mode.

namespace SIMDTraitsDetail
{
	template <typename T>
	inline constexpr bool always_false_v = false;
}

template <typename T, FieldWidth WIDTH>
struct SIMDTraits
{
	static_assert(WIDTH == FieldWidth::Scalar,
				  "SIMDTraits: no Wide/WideMask specialization for this type on the active "
				  "SIMD ISA. Add a specialization in SIMDTraits.h or restrict the field to "
				  "FieldWidth::Scalar.");
	using VecType = T;
};

// ===========================================================================
// AVX2 backend (8 lanes / 32-bit element)
// ===========================================================================
#ifdef TNX_SIMD_AVX2

// --- Count mask helper (always 32-bit integer vector) -----------------------
namespace FieldProxyConsts
{
	static const __m256i element_indices = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
}

// --- FieldMask --------------------------------------------------------------
template <typename FieldType, typename VecType, FieldWidth WIDTH>
struct FieldMask
{
	VecType mask;
	using Traits = SIMDTraits<FieldType, WIDTH>;

	// Three input forms: a VecType vector, a FieldProxy lvalue (load lane data),
	// or a scalar FieldType (broadcast). Preserved verbatim from the pre-split
	// implementation — same branching, same blend op (AVX2 _ps blend, used for
	// all element types in the legacy code path).
	template <typename TVAL, typename FVAL>
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

		if constexpr (std::is_same_v<VecType, __m256>)
		{
			return _mm256_blendv_ps(falseV, trueV, mask);
		}
		else // is __m256i
		{
			return _mm256_blendv_epi8(falseV, trueV, mask);
		}
	}

	FieldMask& operator&=(const __m256i other)
	{
		if constexpr (std::is_same_v<VecType, __m256>)
		{
			mask = _mm256_castsi256_ps(other);
		}
		else
		{
			mask = other;
		}
		return *this;
	}
};

// --- float -----------------------------------------------------------------
template <FieldWidth WIDTH>
struct SIMDTraits<float, WIDTH>
{
	using VecType = __m256;
	static FORCE_INLINE VecType load(const float* ptr) { return _mm256_loadu_ps(ptr); }

	static FORCE_INLINE void store(float* ptr, [[maybe_unused]] __m256i mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask)
		{
			_mm256_storeu_ps(ptr, _mm256_blendv_ps(_mm256_loadu_ps(ptr), val, _mm256_castsi256_ps(mask)));
		}
		else { _mm256_storeu_ps(ptr, val); }
	}

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

	// Generate a mask (as __m256i) where the first `count` lanes are active.
	// Used by FieldProxy::Bind to set the per-chunk count mask.
	static FORCE_INLINE __m256i GenerateCountMask(int32_t count)
	{
		const __m256i cnt = _mm256_set1_epi32(count);
		return _mm256_cmpgt_epi32(cnt, FieldProxyConsts::element_indices);
	}

	// OR a scalar value into each lane of the flags array.
	static FORCE_INLINE void StoreFlagsOr(int32_t* flagsPtr, int32_t value)
	{
		__m256i f = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(flagsPtr));
		f         = _mm256_or_si256(f, _mm256_set1_epi32(value));
		_mm256_storeu_si256(reinterpret_cast<__m256i*>(flagsPtr), f);
	}
};

// --- int32_t ----------------------------------------------------------------
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
		alignas(32) int32_t aData[8], bData[8], result[8];
		_mm256_store_si256((__m256i*)aData, a);
		_mm256_store_si256((__m256i*)bData, b);
		for (int i = 0; i < 8; ++i) result[i] = aData[i] / bData[i];
		return _mm256_load_si256((__m256i*)result);
	}

	static FORCE_INLINE FieldMask<int32_t, VecType, WIDTH> GT(VecType a, VecType b) { return {_mm256_cmpgt_epi32(a, b)}; }
	static FORCE_INLINE FieldMask<int32_t, VecType, WIDTH> LT(VecType a, VecType b) { return {_mm256_cmpgt_epi32(b, a)}; }
	static FORCE_INLINE FieldMask<int32_t, VecType, WIDTH> EQ(VecType a, VecType b) { return {_mm256_cmpeq_epi32(a, b)}; }

	static FORCE_INLINE FieldMask<int32_t, VecType, WIDTH> NEQ(VecType a, VecType b)
	{
		return {_mm256_andnot_si256(_mm256_cmpeq_epi32(a, b), _mm256_set1_epi32(-1))};
	}

	static FORCE_INLINE VecType min(VecType a, VecType b) { return _mm256_min_epi32(a, b); }
	static FORCE_INLINE VecType max(VecType a, VecType b) { return _mm256_max_epi32(a, b); }
	static FORCE_INLINE VecType abs(VecType a) { return _mm256_abs_epi32(a); }

	static FORCE_INLINE __m256i GenerateCountMask(int32_t count)
	{
		const __m256i cnt = _mm256_set1_epi32(count);
		return _mm256_cmpgt_epi32(cnt, FieldProxyConsts::element_indices);
	}

	static FORCE_INLINE void StoreFlagsOr(int32_t* flagsPtr, int32_t value)
	{
		__m256i f = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(flagsPtr));
		f         = _mm256_or_si256(f, _mm256_set1_epi32(value));
		_mm256_storeu_si256(reinterpret_cast<__m256i*>(flagsPtr), f);
	}
};

// --- uint32_t (delegates to int32_t intrinsics) ----------------------------
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

	static FORCE_INLINE FieldMask<uint32_t, VecType, WIDTH> GT(VecType a, VecType b) { return {_mm256_cmpgt_epi32(a, b)}; }
	static FORCE_INLINE FieldMask<uint32_t, VecType, WIDTH> LT(VecType a, VecType b) { return {_mm256_cmpgt_epi32(b, a)}; }
	static FORCE_INLINE FieldMask<uint32_t, VecType, WIDTH> EQ(VecType a, VecType b) { return {_mm256_cmpeq_epi32(a, b)}; }

	static FORCE_INLINE FieldMask<uint32_t, VecType, WIDTH> NEQ(VecType a, VecType b)
	{
		return {_mm256_andnot_si256(_mm256_cmpeq_epi32(a, b), _mm256_set1_epi32(-1))};
	}

	static FORCE_INLINE VecType min(VecType a, VecType b) { return _mm256_min_epu32(a, b); }
	static FORCE_INLINE VecType max(VecType a, VecType b) { return _mm256_max_epu32(a, b); }

	static FORCE_INLINE __m256i GenerateCountMask(int32_t count)
	{
		return SIMDTraits<int32_t, WIDTH>::GenerateCountMask(count);
	}

	static FORCE_INLINE void StoreFlagsOr(int32_t* flagsPtr, int32_t value)
	{
		SIMDTraits<int32_t, WIDTH>::StoreFlagsOr(flagsPtr, value);
	}
};

// --- Fixed32 (add/sub/cmp delegate to int32_t; mul/div scalar fallback) ----
template <FieldWidth WIDTH>
struct SIMDTraits<Fixed32, WIDTH>
{
	using VecType = __m256i;
	using IntT    = SIMDTraits<int32_t, WIDTH>;

	static FORCE_INLINE VecType load(const Fixed32* ptr)
	{
		return IntT::load(reinterpret_cast<const int32_t*>(ptr));
	}

	static FORCE_INLINE void store(Fixed32* ptr, __m256i mask, VecType val)
	{
		IntT::store(reinterpret_cast<int32_t*>(ptr), mask, val);
	}

	static FORCE_INLINE void stream(Fixed32* ptr, __m256i mask, VecType val)
	{
		IntT::stream(reinterpret_cast<int32_t*>(ptr), mask, val);
	}

	static FORCE_INLINE VecType set1(Fixed32 val) { return IntT::set1(val.value); }
	static FORCE_INLINE VecType add(VecType a, VecType b) { return IntT::add(a, b); }
	static FORCE_INLINE VecType sub(VecType a, VecType b) { return IntT::sub(a, b); }

	// TODO: vectorize using _mm256_mul_epi32 (4-lane 32x32→64) twice per 8-lane
	// pass, then divide by Fixed32::Scale. For now, scalar-fallback keeps the
	// decimal-scale arithmetic obvious and bit-identical to the scalar path.
	static FORCE_INLINE VecType mul(VecType a, VecType b)
	{
		alignas(32) int32_t aData[8], bData[8], result[8];
		_mm256_store_si256((__m256i*)aData, a);
		_mm256_store_si256((__m256i*)bData, b);
		for (int i = 0; i < 8; ++i)
		{
			int64_t product = static_cast<int64_t>(aData[i]) * bData[i];
			result[i]       = static_cast<int32_t>(product / Fixed32::Scale64);
		}
		return _mm256_load_si256((__m256i*)result);
	}

	// TODO: same vectorization opportunity. Scalar-fallback for now.
	static FORCE_INLINE VecType div(VecType a, VecType b)
	{
		alignas(32) int32_t aData[8], bData[8], result[8];
		_mm256_store_si256((__m256i*)aData, a);
		_mm256_store_si256((__m256i*)bData, b);
		for (int i = 0; i < 8; ++i)
		{
			int64_t scaled = static_cast<int64_t>(aData[i]) * Fixed32::Scale64;
			result[i]      = static_cast<int32_t>(scaled / bData[i]);
		}
		return _mm256_load_si256((__m256i*)result);
	}

	static FORCE_INLINE FieldMask<Fixed32, VecType, WIDTH> GT(VecType a, VecType b) { return {_mm256_cmpgt_epi32(a, b)}; }
	static FORCE_INLINE FieldMask<Fixed32, VecType, WIDTH> LT(VecType a, VecType b) { return {_mm256_cmpgt_epi32(b, a)}; }
	static FORCE_INLINE FieldMask<Fixed32, VecType, WIDTH> EQ(VecType a, VecType b) { return {_mm256_cmpeq_epi32(a, b)}; }

	static FORCE_INLINE FieldMask<Fixed32, VecType, WIDTH> NEQ(VecType a, VecType b)
	{
		return {_mm256_andnot_si256(_mm256_cmpeq_epi32(a, b), _mm256_set1_epi32(-1))};
	}

	static FORCE_INLINE VecType min(VecType a, VecType b) { return IntT::min(a, b); }
	static FORCE_INLINE VecType max(VecType a, VecType b) { return IntT::max(a, b); }
	static FORCE_INLINE VecType abs(VecType a) { return _mm256_abs_epi32(a); }

	static FORCE_INLINE __m256i GenerateCountMask(int32_t count)
	{
		return IntT::GenerateCountMask(count);
	}

	static FORCE_INLINE void StoreFlagsOr(int32_t* flagsPtr, int32_t value)
	{
		IntT::StoreFlagsOr(flagsPtr, value);
	}
};

#endif // TNX_SIMD_AVX2

// ===========================================================================
// AVX-512 backend (16 lanes / 32-bit element) — STUB
// ===========================================================================
#ifdef TNX_SIMD_AVX512
template <FieldWidth WIDTH>
struct SIMDTraits<float, WIDTH>
{
	static_assert(SIMDTraitsDetail::always_false_v<float>,
				  "AVX-512 SIMDTraits<float> not yet implemented. Fill in this specialization.");
};
template <FieldWidth WIDTH>
struct SIMDTraits<int32_t, WIDTH>
{
	static_assert(SIMDTraitsDetail::always_false_v<int32_t>,
				  "AVX-512 SIMDTraits<int32_t> not yet implemented. Fill in this specialization.");
};
template <FieldWidth WIDTH>
struct SIMDTraits<uint32_t, WIDTH>
{
	static_assert(SIMDTraitsDetail::always_false_v<uint32_t>,
				  "AVX-512 SIMDTraits<uint32_t> not yet implemented. Fill in this specialization.");
};
template <FieldWidth WIDTH>
struct SIMDTraits<Fixed32, WIDTH>
{
	static_assert(SIMDTraitsDetail::always_false_v<Fixed32>,
				  "AVX-512 SIMDTraits<Fixed32> not yet implemented. Fill in this specialization.");
};
#endif // TNX_SIMD_AVX512

// ===========================================================================
// NEON backend (4 lanes / 32-bit element) — STUB
// ===========================================================================
#ifdef TNX_SIMD_NEON
template <FieldWidth WIDTH>
struct SIMDTraits<float, WIDTH>
{
	static_assert(SIMDTraitsDetail::always_false_v<float>,
				  "NEON SIMDTraits<float> not yet implemented. Fill in this specialization.");
};
template <FieldWidth WIDTH>
struct SIMDTraits<int32_t, WIDTH>
{
	static_assert(SIMDTraitsDetail::always_false_v<int32_t>,
				  "NEON SIMDTraits<int32_t> not yet implemented. Fill in this specialization.");
};
template <FieldWidth WIDTH>
struct SIMDTraits<uint32_t, WIDTH>
{
	static_assert(SIMDTraitsDetail::always_false_v<uint32_t>,
				  "NEON SIMDTraits<uint32_t> not yet implemented. Fill in this specialization.");
};
template <FieldWidth WIDTH>
struct SIMDTraits<Fixed32, WIDTH>
{
	static_assert(SIMDTraitsDetail::always_false_v<Fixed32>,
				  "NEON SIMDTraits<Fixed32> not yet implemented. Fill in this specialization.");
};
#endif // TNX_SIMD_NEON
