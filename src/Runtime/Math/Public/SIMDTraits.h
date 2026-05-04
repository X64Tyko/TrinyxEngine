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

// --- Wide mask element type per ISA ----------------------------------------
// WideMaskType is the type of the mask argument passed to store()/stream() and
// returned by GenerateCountMask(). In AVX2 it is __m256i (a full vector of
// sign-extended lane predicates); in AVX-512 it is __mmask16 (a k-register).
// SimFloatImpl wrappers and FieldProxyMask use this alias so they compile
// correctly under both ISAs without sprinkling #ifdefs into each call site.
#if defined(TNX_SIMD_AVX512)
using WideMaskType = __mmask16;
inline __mmask16 AllLanesActiveMask() { return static_cast<__mmask16>(0xFFFF); }
#elif defined(TNX_SIMD_AVX2)
using WideMaskType = __m256i;
inline __m256i AllLanesActiveMask() { return _mm256_set1_epi64x(-1); }
#else
using WideMaskType = int32_t;
inline int32_t AllLanesActiveMask() { return -1; }
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

// --- Specializations for SimFloatImpl<T> (delegate to underlying type) -----

template <FieldWidth WIDTH>
struct SIMDTraits<SimFloatImpl<float>, WIDTH>
{
	using Underlying = SIMDTraits<float, WIDTH>;
	using VecType    = typename Underlying::VecType; // __m256 (AVX2) or __m512 (AVX-512)

	static FORCE_INLINE VecType load(const SimFloatImpl<float>* ptr)
	{
		return Underlying::load(reinterpret_cast<const float*>(ptr));
	}

	static FORCE_INLINE void store(SimFloatImpl<float>* ptr, WideMaskType mask, VecType val)
	{
		Underlying::store(reinterpret_cast<float*>(ptr), mask, val);
	}

	static FORCE_INLINE void stream(SimFloatImpl<float>* ptr, WideMaskType mask, VecType val)
	{
		Underlying::stream(reinterpret_cast<float*>(ptr), mask, val);
	}

	static FORCE_INLINE VecType set1(float val) { return Underlying::set1(val); }
	static FORCE_INLINE VecType set1(SimFloatImpl<float> val) { return Underlying::set1(val.value); }
	static FORCE_INLINE VecType add(VecType a, VecType b) { return Underlying::add(a, b); }
	static FORCE_INLINE VecType sub(VecType a, VecType b) { return Underlying::sub(a, b); }
	static FORCE_INLINE VecType mul(VecType a, VecType b) { return Underlying::mul(a, b); }
	static FORCE_INLINE VecType div(VecType a, VecType b) { return Underlying::div(a, b); }

	static FORCE_INLINE FieldMask<SimFloatImpl<float>, VecType, WIDTH> GT(VecType a, VecType b)
	{
		return {Underlying::GT(a, b).mask};
	}

	static FORCE_INLINE FieldMask<SimFloatImpl<float>, VecType, WIDTH> LT(VecType a, VecType b)
	{
		return {Underlying::LT(a, b).mask};
	}

	static FORCE_INLINE FieldMask<SimFloatImpl<float>, VecType, WIDTH> EQ(VecType a, VecType b)
	{
		return {Underlying::EQ(a, b).mask};
	}

	static FORCE_INLINE FieldMask<SimFloatImpl<float>, VecType, WIDTH> NEQ(VecType a, VecType b)
	{
		return {Underlying::NEQ(a, b).mask};
	}

	static FORCE_INLINE FieldMask<SimFloatImpl<float>, VecType, WIDTH> GE(VecType a, VecType b)
	{
		return {Underlying::GE(a, b).mask};
	}

	static FORCE_INLINE FieldMask<SimFloatImpl<float>, VecType, WIDTH> LE(VecType a, VecType b)
	{
		return {Underlying::LE(a, b).mask};
	}

	static FORCE_INLINE VecType Blend(VecType a, VecType b) { return Underlying::Blend(a, b); }
	static FORCE_INLINE VecType min(VecType a, VecType b) { return Underlying::min(a, b); }
	static FORCE_INLINE VecType max(VecType a, VecType b) { return Underlying::max(a, b); }
	static FORCE_INLINE VecType abs(VecType a) { return Underlying::abs(a); }
	static FORCE_INLINE WideMaskType GenerateCountMask(int32_t count) { return Underlying::GenerateCountMask(count); }
	static FORCE_INLINE void StoreFlagsOr(int32_t* flagsPtr, int32_t value) { Underlying::StoreFlagsOr(flagsPtr, value); }
};

template <FieldWidth WIDTH>
struct SIMDTraits<SimFloatImpl<Fixed32>, WIDTH>
{
	using Underlying = SIMDTraits<Fixed32, WIDTH>;
	using VecType    = typename Underlying::VecType; // __m256i (AVX2) or __m512i (AVX-512)

	static FORCE_INLINE VecType load(const SimFloatImpl<Fixed32>* ptr)
	{
		return Underlying::load(reinterpret_cast<const Fixed32*>(ptr));
	}

	static FORCE_INLINE void store(SimFloatImpl<Fixed32>* ptr, WideMaskType mask, VecType val)
	{
		Underlying::store(reinterpret_cast<Fixed32*>(ptr), mask, val);
	}

	static FORCE_INLINE void stream(SimFloatImpl<Fixed32>* ptr, WideMaskType mask, VecType val)
	{
		Underlying::stream(reinterpret_cast<Fixed32*>(ptr), mask, val);
	}

	static FORCE_INLINE VecType set1(Fixed32 val) { return Underlying::set1(val); }
	static FORCE_INLINE VecType set1(SimFloatImpl<Fixed32> val) { return Underlying::set1(val.value); }
	static FORCE_INLINE VecType add(VecType a, VecType b) { return Underlying::add(a, b); }
	static FORCE_INLINE VecType sub(VecType a, VecType b) { return Underlying::sub(a, b); }
	static FORCE_INLINE VecType mul(VecType a, VecType b) { return Underlying::mul(a, b); }
	static FORCE_INLINE VecType div(VecType a, VecType b) { return Underlying::div(a, b); }

	static FORCE_INLINE FieldMask<SimFloatImpl<Fixed32>, VecType, WIDTH> GT(VecType a, VecType b)
	{
		return {Underlying::GT(a, b).mask};
	}

	static FORCE_INLINE FieldMask<SimFloatImpl<Fixed32>, VecType, WIDTH> LT(VecType a, VecType b)
	{
		return {Underlying::LT(a, b).mask};
	}

	static FORCE_INLINE FieldMask<SimFloatImpl<Fixed32>, VecType, WIDTH> EQ(VecType a, VecType b)
	{
		return {Underlying::EQ(a, b).mask};
	}

	static FORCE_INLINE FieldMask<SimFloatImpl<Fixed32>, VecType, WIDTH> NEQ(VecType a, VecType b)
	{
		return {Underlying::NEQ(a, b).mask};
	}

	static FORCE_INLINE FieldMask<SimFloatImpl<Fixed32>, VecType, WIDTH> GE(VecType a, VecType b)
	{
		return {Underlying::GE(a, b).mask};
	}

	static FORCE_INLINE FieldMask<SimFloatImpl<Fixed32>, VecType, WIDTH> LE(VecType a, VecType b)
	{
		return {Underlying::LE(a, b).mask};
	}

	// Blend is not available for Fixed32 (int vector) – omit.
	static FORCE_INLINE VecType min(VecType a, VecType b) { return Underlying::min(a, b); }
	static FORCE_INLINE VecType max(VecType a, VecType b) { return Underlying::max(a, b); }
	static FORCE_INLINE VecType abs(VecType a) { return Underlying::abs(a); }
	static FORCE_INLINE WideMaskType GenerateCountMask(int32_t count) { return Underlying::GenerateCountMask(count); }
	static FORCE_INLINE void StoreFlagsOr(int32_t* flagsPtr, int32_t value) { Underlying::StoreFlagsOr(flagsPtr, value); }
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
		else if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<FVAL>>::value) falseV = Traits::load(&FalseVal.WriteArray[FalseVal.index]);
		else if constexpr (std::is_same_v<FVAL, std::remove_cvref_t<FieldType>>) falseV = Traits::set1(FalseVal);
		else falseV                                                                     = Traits::set1(FalseVal);

		VecType trueV;
		if constexpr (std::is_same_v<TVAL, VecType>) trueV = TrueVal;
		else if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<TVAL>>::value) trueV = Traits::load(&TrueVal.WriteArray[TrueVal.index]);
		else if constexpr (std::is_same_v<TVAL, std::remove_cvref_t<FieldType>>) trueV = Traits::set1(TrueVal);
		else trueV                                                                     = Traits::set1(TrueVal);

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
	static FORCE_INLINE VecType sqrt(VecType a) { return _mm256_sqrt_ps(a); }
	static FORCE_INLINE VecType rsqrt(VecType a) { return _mm256_rsqrt_ps(a); }

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

	static FORCE_INLINE FieldMask<int32_t, VecType, WIDTH> GE(VecType a, VecType b)
	{
		return {_mm256_andnot_si256(_mm256_cmpgt_epi32(b, a), _mm256_set1_epi32(-1))};
	}

	static FORCE_INLINE FieldMask<int32_t, VecType, WIDTH> LE(VecType a, VecType b)
	{
		return {_mm256_andnot_si256(_mm256_cmpgt_epi32(a, b), _mm256_set1_epi32(-1))};
	}

	static FORCE_INLINE VecType min(VecType a, VecType b) { return _mm256_min_epi32(a, b); }
	static FORCE_INLINE VecType max(VecType a, VecType b) { return _mm256_max_epi32(a, b); }
	static FORCE_INLINE VecType abs(VecType a) { return _mm256_abs_epi32(a); }

	static FORCE_INLINE VecType sqrt(VecType a)
	{
		alignas(32) int32_t aData[8], result[8];
		_mm256_store_si256((__m256i*)aData, a);
		for (int i = 0; i < 8; ++i) result[i] = FixedSqrt(Fixed32::FromRaw(aData[i])).Raw();
		return _mm256_load_si256((__m256i*)result);
	}

	static FORCE_INLINE VecType rsqrt(VecType a)
	{
		alignas(32) int32_t aData[8], result[8];
		_mm256_store_si256((__m256i*)aData, a);
		for (int i = 0; i < 8; ++i)
		{
			Fixed32 x = Fixed32::FromRaw(aData[i]);
			if (x.Raw() <= 0)
			{
				result[i] = 0;
				continue;
			}
			Fixed32 s = FixedSqrt(x);
			result[i] = (Fixed32::FromFloat(1.0f) / s).Raw();
		}
		return _mm256_load_si256((__m256i*)result);
	}

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

	// --- Bitwise / shift helpers (used by flag-sweep loops in Registry) -----
	static FORCE_INLINE VecType bitand_(VecType a, VecType b) { return _mm256_and_si256(a, b); }
	static FORCE_INLINE VecType bitandnot(VecType a, VecType b) { return _mm256_andnot_si256(a, b); } // ~a & b
	static FORCE_INLINE VecType bitor_(VecType a, VecType b) { return _mm256_or_si256(a, b); }
	static FORCE_INLINE VecType srl(VecType a, int count) { return _mm256_srli_epi32(a, count); }

	static FORCE_INLINE int hsum(VecType v)
	{
		__m128i lo  = _mm256_castsi256_si128(v);
		__m128i hi  = _mm256_extracti128_si256(v, 1);
		__m128i s   = _mm_add_epi32(lo, hi);
		s = _mm_add_epi32(s, _mm_shuffle_epi32(s, _MM_SHUFFLE(2, 3, 0, 1)));
		return _mm_cvtsi128_si32(_mm_add_epi32(s, _mm_shuffle_epi32(s, _MM_SHUFFLE(1, 0, 3, 2))));
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

	static FORCE_INLINE FieldMask<uint32_t, VecType, WIDTH> GE(VecType a, VecType b)
	{
		// Flip sign bit to convert unsigned compare to signed
		const __m256i signBit = _mm256_set1_epi32(0x80000000);
		return {_mm256_cmpgt_epi32(_mm256_xor_si256(a, signBit), _mm256_xor_si256(b, signBit))};
	}

	static FORCE_INLINE FieldMask<uint32_t, VecType, WIDTH> LE(VecType a, VecType b)
	{
		return {_mm256_andnot_si256(GE(a, b).mask, _mm256_set1_epi32(-1))};
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
// AVX-512 backend (16 lanes / 32-bit element)
// ===========================================================================
#ifdef TNX_SIMD_AVX512

// --- FieldMask (AVX-512) ----------------------------------------------------
// Comparisons return a k-register (__mmask16) rather than a full-width vector,
// so the mask field is __mmask16 regardless of whether FieldType is float or int.
template <typename FieldType, typename VecType, FieldWidth WIDTH>
struct FieldMask
{
	__mmask16 mask;
	using Traits = SIMDTraits<FieldType, WIDTH>;

	template <typename TVAL, typename FVAL>
	FORCE_INLINE decltype(auto) Choose(TVAL&& TrueVal, FVAL&& FalseVal) const
	{
		VecType falseV;
		if constexpr (std::is_same_v<FVAL, VecType>) falseV = FalseVal;
		else if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<FVAL>>::value) falseV = Traits::load(&FalseVal.WriteArray[FalseVal.index]);
		else if constexpr (std::is_same_v<FVAL, std::remove_cvref_t<FieldType>>) falseV = Traits::set1(FalseVal);
		else falseV                                                                     = Traits::set1(FalseVal);

		VecType trueV;
		if constexpr (std::is_same_v<TVAL, VecType>) trueV = TrueVal;
		else if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<TVAL>>::value) trueV = Traits::load(&TrueVal.WriteArray[TrueVal.index]);
		else if constexpr (std::is_same_v<TVAL, std::remove_cvref_t<FieldType>>) trueV = Traits::set1(TrueVal);
		else trueV                                                                     = Traits::set1(TrueVal);

		if constexpr (std::is_same_v<VecType, __m512>)
			return _mm512_mask_blend_ps(mask, falseV, trueV);
		else
			return _mm512_mask_blend_epi32(mask, falseV, trueV);
	}

	FieldMask& operator&=(const __mmask16 other)
	{
		mask = mask & other;
		return *this;
	}
};

// --- Shared helpers ---------------------------------------------------------
namespace FieldProxyConsts512
{
	// GenerateCountMask: low `count` bits set, rest clear.
	FORCE_INLINE __mmask16 CountMask(int32_t count)
	{
		return static_cast<__mmask16>((1u << count) - 1);
	}

	FORCE_INLINE void StoreFlagsOr16(int32_t* flagsPtr, int32_t value)
	{
		__m512i f = _mm512_loadu_si512(static_cast<const void*>(flagsPtr));
		f         = _mm512_or_si512(f, _mm512_set1_epi32(value));
		_mm512_storeu_si512(static_cast<void*>(flagsPtr), f);
	}
}

// --- float ------------------------------------------------------------------
template <FieldWidth WIDTH>
struct SIMDTraits<float, WIDTH>
{
	using VecType = __m512;

	static FORCE_INLINE VecType load(const float* ptr) { return _mm512_loadu_ps(ptr); }

	static FORCE_INLINE void store(float* ptr, [[maybe_unused]] __mmask16 mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask)
			_mm512_mask_storeu_ps(static_cast<void*>(ptr), mask, val);
		else
			_mm512_storeu_ps(static_cast<void*>(ptr), val);
	}

	static FORCE_INLINE void stream(float* ptr, [[maybe_unused]] __mmask16 mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask)
			_mm512_mask_storeu_ps(static_cast<void*>(ptr), mask, val);
		else
			_mm512_stream_ps(static_cast<void*>(ptr), val);
	}

	static FORCE_INLINE VecType set1(float val) { return _mm512_set1_ps(val); }
	static FORCE_INLINE VecType add(VecType a, VecType b) { return _mm512_add_ps(a, b); }
	static FORCE_INLINE VecType sub(VecType a, VecType b) { return _mm512_sub_ps(a, b); }
	static FORCE_INLINE VecType mul(VecType a, VecType b) { return _mm512_mul_ps(a, b); }
	static FORCE_INLINE VecType div(VecType a, VecType b) { return _mm512_div_ps(a, b); }

	static FORCE_INLINE FieldMask<float, VecType, WIDTH> GT(VecType a, VecType b)  { return {_mm512_cmp_ps_mask(a, b, _CMP_GT_OQ)}; }
	static FORCE_INLINE FieldMask<float, VecType, WIDTH> LT(VecType a, VecType b)  { return {_mm512_cmp_ps_mask(a, b, _CMP_LT_OQ)}; }
	static FORCE_INLINE FieldMask<float, VecType, WIDTH> GE(VecType a, VecType b)  { return {_mm512_cmp_ps_mask(a, b, _CMP_GE_OQ)}; }
	static FORCE_INLINE FieldMask<float, VecType, WIDTH> LE(VecType a, VecType b)  { return {_mm512_cmp_ps_mask(a, b, _CMP_LE_OQ)}; }
	static FORCE_INLINE FieldMask<float, VecType, WIDTH> EQ(VecType a, VecType b)  { return {_mm512_cmp_ps_mask(a, b, _CMP_EQ_OQ)}; }
	static FORCE_INLINE FieldMask<float, VecType, WIDTH> NEQ(VecType a, VecType b) { return {_mm512_cmp_ps_mask(a, b, _CMP_NEQ_OQ)}; }

	static FORCE_INLINE VecType Blend(VecType a, VecType b)
	{
		return _mm512_mask_blend_ps(_mm512_cmp_ps_mask(a, b, _CMP_GT_OQ), a, b);
	}

	static FORCE_INLINE VecType min(VecType a, VecType b)   { return _mm512_min_ps(a, b); }
	static FORCE_INLINE VecType max(VecType a, VecType b)   { return _mm512_max_ps(a, b); }
	static FORCE_INLINE VecType abs(VecType a)               { return _mm512_abs_ps(a); }
	static FORCE_INLINE VecType sqrt(VecType a)              { return _mm512_sqrt_ps(a); }
	static FORCE_INLINE VecType rsqrt(VecType a)             { return _mm512_rsqrt14_ps(a); }

	static FORCE_INLINE __mmask16 GenerateCountMask(int32_t count) { return FieldProxyConsts512::CountMask(count); }
	static FORCE_INLINE void StoreFlagsOr(int32_t* flagsPtr, int32_t value) { FieldProxyConsts512::StoreFlagsOr16(flagsPtr, value); }
};

// --- int32_t ----------------------------------------------------------------
template <FieldWidth WIDTH>
struct SIMDTraits<int32_t, WIDTH>
{
	using VecType = __m512i;

	static FORCE_INLINE VecType load(const int32_t* ptr) { return _mm512_loadu_si512(static_cast<const void*>(ptr)); }

	static FORCE_INLINE void store(int32_t* ptr, [[maybe_unused]] __mmask16 mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask)
			_mm512_mask_storeu_epi32(static_cast<void*>(ptr), mask, val);
		else
			_mm512_storeu_si512(static_cast<void*>(ptr), val);
	}

	static FORCE_INLINE void stream(int32_t* ptr, [[maybe_unused]] __mmask16 mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask)
			_mm512_mask_storeu_epi32(static_cast<void*>(ptr), mask, val);
		else
			_mm512_stream_si512(static_cast<void*>(ptr), val);
	}

	static FORCE_INLINE VecType set1(int32_t val) { return _mm512_set1_epi32(val); }
	static FORCE_INLINE VecType add(VecType a, VecType b) { return _mm512_add_epi32(a, b); }
	static FORCE_INLINE VecType sub(VecType a, VecType b) { return _mm512_sub_epi32(a, b); }
	static FORCE_INLINE VecType mul(VecType a, VecType b) { return _mm512_mullo_epi32(a, b); }

	static FORCE_INLINE VecType div(VecType a, VecType b)
	{
		alignas(64) int32_t aData[16], bData[16], result[16];
		_mm512_store_si512(static_cast<void*>(aData), a);
		_mm512_store_si512(static_cast<void*>(bData), b);
		for (int i = 0; i < 16; ++i) result[i] = aData[i] / bData[i];
		return _mm512_load_si512(static_cast<const void*>(result));
	}

	static FORCE_INLINE FieldMask<int32_t, VecType, WIDTH> GT(VecType a, VecType b)  { return {_mm512_cmpgt_epi32_mask(a, b)}; }
	static FORCE_INLINE FieldMask<int32_t, VecType, WIDTH> LT(VecType a, VecType b)  { return {_mm512_cmpgt_epi32_mask(b, a)}; }
	static FORCE_INLINE FieldMask<int32_t, VecType, WIDTH> EQ(VecType a, VecType b)  { return {_mm512_cmpeq_epi32_mask(a, b)}; }
	static FORCE_INLINE FieldMask<int32_t, VecType, WIDTH> NEQ(VecType a, VecType b) { return {_mm512_cmpneq_epi32_mask(a, b)}; }
	static FORCE_INLINE FieldMask<int32_t, VecType, WIDTH> GE(VecType a, VecType b)  { return {_mm512_cmpge_epi32_mask(a, b)}; }
	static FORCE_INLINE FieldMask<int32_t, VecType, WIDTH> LE(VecType a, VecType b)  { return {_mm512_cmple_epi32_mask(a, b)}; }

	static FORCE_INLINE VecType min(VecType a, VecType b) { return _mm512_min_epi32(a, b); }
	static FORCE_INLINE VecType max(VecType a, VecType b) { return _mm512_max_epi32(a, b); }
	static FORCE_INLINE VecType abs(VecType a)             { return _mm512_abs_epi32(a); }

	static FORCE_INLINE VecType sqrt(VecType a)
	{
		alignas(64) int32_t aData[16], result[16];
		_mm512_store_si512(static_cast<void*>(aData), a);
		for (int i = 0; i < 16; ++i) result[i] = FixedSqrt(Fixed32::FromRaw(aData[i])).Raw();
		return _mm512_load_si512(static_cast<const void*>(result));
	}

	static FORCE_INLINE VecType rsqrt(VecType a)
	{
		alignas(64) int32_t aData[16], result[16];
		_mm512_store_si512(static_cast<void*>(aData), a);
		for (int i = 0; i < 16; ++i)
		{
			Fixed32 x = Fixed32::FromRaw(aData[i]);
			if (x.Raw() <= 0) { result[i] = 0; continue; }
			Fixed32 s = FixedSqrt(x);
			result[i] = (Fixed32::FromFloat(1.0f) / s).Raw();
		}
		return _mm512_load_si512(static_cast<const void*>(result));
	}

	static FORCE_INLINE __mmask16 GenerateCountMask(int32_t count) { return FieldProxyConsts512::CountMask(count); }
	static FORCE_INLINE void StoreFlagsOr(int32_t* flagsPtr, int32_t value) { FieldProxyConsts512::StoreFlagsOr16(flagsPtr, value); }

	// --- Bitwise / shift helpers (used by flag-sweep loops in Registry) -----
	static FORCE_INLINE VecType bitand_(VecType a, VecType b) { return _mm512_and_si512(a, b); }
	static FORCE_INLINE VecType bitandnot(VecType a, VecType b) { return _mm512_andnot_si512(a, b); } // ~a & b
	static FORCE_INLINE VecType bitor_(VecType a, VecType b) { return _mm512_or_si512(a, b); }
	static FORCE_INLINE VecType srl(VecType a, int count) { return _mm512_srli_epi32(a, count); }

	static FORCE_INLINE int hsum(VecType v)
	{
		__m256i lo  = _mm512_castsi512_si256(v);
		__m256i hi  = _mm512_extracti64x4_epi64(v, 1);
		__m256i s   = _mm256_add_epi32(lo, hi);
		__m128i lo2 = _mm256_castsi256_si128(s);
		__m128i hi2 = _mm256_extracti128_si256(s, 1);
		__m128i s2  = _mm_add_epi32(lo2, hi2);
		s2 = _mm_add_epi32(s2, _mm_shuffle_epi32(s2, _MM_SHUFFLE(2, 3, 0, 1)));
		return _mm_cvtsi128_si32(_mm_add_epi32(s2, _mm_shuffle_epi32(s2, _MM_SHUFFLE(1, 0, 3, 2))));
	}
};

// --- uint32_t ---------------------------------------------------------------
template <FieldWidth WIDTH>
struct SIMDTraits<uint32_t, WIDTH>
{
	using VecType = __m512i;
	using IntT    = SIMDTraits<int32_t, WIDTH>;

	static FORCE_INLINE VecType load(const uint32_t* ptr) { return _mm512_loadu_si512(static_cast<const void*>(ptr)); }

	static FORCE_INLINE void store(uint32_t* ptr, [[maybe_unused]] __mmask16 mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask)
			_mm512_mask_storeu_epi32(static_cast<void*>(ptr), mask, val);
		else
			_mm512_storeu_si512(static_cast<void*>(ptr), val);
	}

	static FORCE_INLINE void stream(uint32_t* ptr, [[maybe_unused]] __mmask16 mask, VecType val)
	{
		if constexpr (WIDTH == FieldWidth::WideMask)
			_mm512_mask_storeu_epi32(static_cast<void*>(ptr), mask, val);
		else
			_mm512_stream_si512(static_cast<void*>(ptr), val);
	}

	static FORCE_INLINE VecType set1(uint32_t val) { return _mm512_set1_epi32(static_cast<int32_t>(val)); }
	static FORCE_INLINE VecType add(VecType a, VecType b) { return _mm512_add_epi32(a, b); }
	static FORCE_INLINE VecType sub(VecType a, VecType b) { return _mm512_sub_epi32(a, b); }
	static FORCE_INLINE VecType mul(VecType a, VecType b) { return _mm512_mullo_epi32(a, b); }

	static FORCE_INLINE VecType div(VecType a, VecType b)
	{
		alignas(64) uint32_t aData[16], bData[16], result[16];
		_mm512_store_si512(static_cast<void*>(aData), a);
		_mm512_store_si512(static_cast<void*>(bData), b);
		for (int i = 0; i < 16; ++i) result[i] = aData[i] / bData[i];
		return _mm512_load_si512(static_cast<const void*>(result));
	}

	static FORCE_INLINE FieldMask<uint32_t, VecType, WIDTH> GT(VecType a, VecType b)  { return {_mm512_cmpgt_epu32_mask(a, b)}; }
	static FORCE_INLINE FieldMask<uint32_t, VecType, WIDTH> LT(VecType a, VecType b)  { return {_mm512_cmplt_epu32_mask(a, b)}; }
	static FORCE_INLINE FieldMask<uint32_t, VecType, WIDTH> EQ(VecType a, VecType b)  { return {_mm512_cmpeq_epu32_mask(a, b)}; }
	static FORCE_INLINE FieldMask<uint32_t, VecType, WIDTH> NEQ(VecType a, VecType b) { return {_mm512_cmpneq_epu32_mask(a, b)}; }
	static FORCE_INLINE FieldMask<uint32_t, VecType, WIDTH> GE(VecType a, VecType b)  { return {_mm512_cmpge_epu32_mask(a, b)}; }
	static FORCE_INLINE FieldMask<uint32_t, VecType, WIDTH> LE(VecType a, VecType b)  { return {_mm512_cmple_epu32_mask(a, b)}; }

	static FORCE_INLINE VecType min(VecType a, VecType b) { return _mm512_min_epu32(a, b); }
	static FORCE_INLINE VecType max(VecType a, VecType b) { return _mm512_max_epu32(a, b); }

	static FORCE_INLINE __mmask16 GenerateCountMask(int32_t count) { return IntT::GenerateCountMask(count); }
	static FORCE_INLINE void StoreFlagsOr(int32_t* flagsPtr, int32_t value) { IntT::StoreFlagsOr(flagsPtr, value); }
};

// --- Fixed32 (add/sub/cmp delegate to int32_t; mul/div scalar fallback) ----
template <FieldWidth WIDTH>
struct SIMDTraits<Fixed32, WIDTH>
{
	using VecType = __m512i;
	using IntT    = SIMDTraits<int32_t, WIDTH>;

	static FORCE_INLINE VecType load(const Fixed32* ptr)
	{
		return IntT::load(reinterpret_cast<const int32_t*>(ptr));
	}

	static FORCE_INLINE void store(Fixed32* ptr, __mmask16 mask, VecType val)
	{
		IntT::store(reinterpret_cast<int32_t*>(ptr), mask, val);
	}

	static FORCE_INLINE void stream(Fixed32* ptr, __mmask16 mask, VecType val)
	{
		IntT::stream(reinterpret_cast<int32_t*>(ptr), mask, val);
	}

	static FORCE_INLINE VecType set1(Fixed32 val) { return IntT::set1(val.value); }
	static FORCE_INLINE VecType add(VecType a, VecType b) { return IntT::add(a, b); }
	static FORCE_INLINE VecType sub(VecType a, VecType b) { return IntT::sub(a, b); }

	static FORCE_INLINE VecType mul(VecType a, VecType b)
	{
		alignas(64) int32_t aData[16], bData[16], result[16];
		_mm512_store_si512(static_cast<void*>(aData), a);
		_mm512_store_si512(static_cast<void*>(bData), b);
		for (int i = 0; i < 16; ++i)
		{
			int64_t product = static_cast<int64_t>(aData[i]) * bData[i];
			result[i]       = static_cast<int32_t>(product / Fixed32::Scale64);
		}
		return _mm512_load_si512(static_cast<const void*>(result));
	}

	static FORCE_INLINE VecType div(VecType a, VecType b)
	{
		alignas(64) int32_t aData[16], bData[16], result[16];
		_mm512_store_si512(static_cast<void*>(aData), a);
		_mm512_store_si512(static_cast<void*>(bData), b);
		for (int i = 0; i < 16; ++i)
		{
			int64_t scaled = static_cast<int64_t>(aData[i]) * Fixed32::Scale64;
			result[i]      = static_cast<int32_t>(scaled / bData[i]);
		}
		return _mm512_load_si512(static_cast<const void*>(result));
	}

	static FORCE_INLINE FieldMask<Fixed32, VecType, WIDTH> GT(VecType a, VecType b)  { return {_mm512_cmpgt_epi32_mask(a, b)}; }
	static FORCE_INLINE FieldMask<Fixed32, VecType, WIDTH> LT(VecType a, VecType b)  { return {_mm512_cmpgt_epi32_mask(b, a)}; }
	static FORCE_INLINE FieldMask<Fixed32, VecType, WIDTH> EQ(VecType a, VecType b)  { return {_mm512_cmpeq_epi32_mask(a, b)}; }
	static FORCE_INLINE FieldMask<Fixed32, VecType, WIDTH> NEQ(VecType a, VecType b) { return {_mm512_cmpneq_epi32_mask(a, b)}; }

	static FORCE_INLINE VecType min(VecType a, VecType b) { return IntT::min(a, b); }
	static FORCE_INLINE VecType max(VecType a, VecType b) { return IntT::max(a, b); }
	static FORCE_INLINE VecType abs(VecType a)             { return _mm512_abs_epi32(a); }

	static FORCE_INLINE __mmask16 GenerateCountMask(int32_t count) { return IntT::GenerateCountMask(count); }
	static FORCE_INLINE void StoreFlagsOr(int32_t* flagsPtr, int32_t value) { IntT::StoreFlagsOr(flagsPtr, value); }
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
