#pragma once
#include <cstdint>
#include <type_traits>
#include <immintrin.h>

#include "Logger.h"

namespace FieldProxyConsts
{
    static const __m256i element_indices = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7); // Indices of elements
    
}

// SIMD type traits for selecting correct intrinsics
template<typename T, bool MASK>
struct SIMDTraits;

template<bool MASK>
struct SIMDTraits<float, MASK>
{
    using VecType = __m256;
    static __forceinline VecType load(const float* ptr) { return _mm256_loadu_ps(ptr); }
    static __forceinline void store(float* ptr, [[maybe_unused]] __m256i mask, VecType val)
    {
        if constexpr (MASK) { _mm256_maskstore_ps(ptr, mask, val); }
        else { _mm256_storeu_ps(ptr, val); }
    }
    static __forceinline VecType set1(float val) { return _mm256_set1_ps(val); }
    static __forceinline VecType add(VecType a, VecType b) { return _mm256_add_ps(a, b); }
    static __forceinline VecType sub(VecType a, VecType b) { return _mm256_sub_ps(a, b); }
    static __forceinline VecType mul(VecType a, VecType b) { return _mm256_mul_ps(a, b); }
    static __forceinline VecType div(VecType a, VecType b) { return _mm256_div_ps(a, b); }
};

template<bool MASK>
struct SIMDTraits<int32_t, MASK>
{
    using VecType = __m256i;
    static __forceinline VecType load(const int32_t* ptr) { return _mm256_loadu_si256((const __m256i*)ptr); }
    static __forceinline void store(int32_t* ptr, [[maybe_unused]] __m256i mask, VecType val)
    {
        if constexpr (MASK) { _mm256_maskstore_epi32(ptr, mask, val); }
        else { _mm256_storeu_si256((__m256i*)ptr, val); }
    }
    static __forceinline VecType set1(int32_t val) { return _mm256_set1_epi32(val); }
    static __forceinline VecType add(VecType a, VecType b) { return _mm256_add_epi32(a, b); }
    static __forceinline VecType sub(VecType a, VecType b) { return _mm256_sub_epi32(a, b); }
    static __forceinline VecType mul(VecType a, VecType b) { return _mm256_mullo_epi32(a, b); }
    static __forceinline VecType div(VecType a, VecType b) {
        // Integer division has no SIMD intrinsic - fall back to scalar
        alignas(32) int32_t aData[8], bData[8], result[8];
        _mm256_store_si256((__m256i*)aData, a);
        _mm256_store_si256((__m256i*)bData, b);
        for (int i = 0; i < 8; ++i) result[i] = aData[i] / bData[i];
        return _mm256_load_si256((__m256i*)result);
    }
};

template<bool MASK>
struct SIMDTraits<uint32_t, MASK>
{
    using VecType = __m256i;
    static __forceinline VecType load(const uint32_t* ptr) { return _mm256_loadu_si256((const __m256i*)ptr); }
    static __forceinline void store(uint32_t* ptr, [[maybe_unused]] __m256i mask, VecType val)
    {
        if constexpr (MASK) { _mm256_maskstore_epi32((int32_t*)ptr, mask, val); }
        else { _mm256_storeu_si256((__m256i*)ptr, val); }
    }
    static __forceinline VecType set1(uint32_t val) { return _mm256_set1_epi32(val); }
    static __forceinline VecType add(VecType a, VecType b) { return _mm256_add_epi32(a, b); }
    static __forceinline VecType sub(VecType a, VecType b) { return _mm256_sub_epi32(a, b); }
    static __forceinline VecType mul(VecType a, VecType b) { return _mm256_mullo_epi32(a, b); }
    static __forceinline VecType div(VecType a, VecType b) {
        alignas(32) uint32_t aData[8], bData[8], result[8];
        _mm256_store_si256((__m256i*)aData, a);
        _mm256_store_si256((__m256i*)bData, b);
        for (int i = 0; i < 8; ++i) result[i] = aData[i] / bData[i];
        return _mm256_load_si256((__m256i*)result);
    }
};

// Helper: Proxy for individual field access with SIMD
template <typename FieldType, bool MASK>
struct FieldProxy
{
    FieldType* __restrict array;
    uint32_t index;
    __m256i mask = _mm256_set1_epi64x(-1);  // Only used when UseMask = true

    FieldProxy(){}

    operator FieldType() const { return array[index]; }

using Traits = SIMDTraits<FieldType, MASK>;
    __forceinline FieldProxy& operator=(FieldType value)
    {
        Traits::store(&array[index], mask, Traits::set1(value));
        return *this;
    }

    __forceinline FieldProxy& operator+=(FieldType value)
    {
        auto vec = Traits::load(&array[index]);
        auto val = Traits::set1(value);
        Traits::store(&array[index], mask, Traits::add(vec, val));
            
        return *this;
    }

    __forceinline FieldProxy& operator-=(FieldType value)
    {
        auto vec = Traits::load(&array[index]);
        auto val = Traits::set1(value);
        Traits::store(&array[index], mask, Traits::sub(vec, val));
        return *this;
    }

    __forceinline FieldProxy& operator*=(FieldType value)
    {
        auto vec = Traits::load(&array[index]);
        auto val = Traits::set1(value);
        Traits::store(&array[index], mask, Traits::mul(vec, val));
        return *this;
    }

    __forceinline FieldProxy& operator/=(FieldType value)
    {
        auto vec = Traits::load(&array[index]);
        auto val = Traits::set1(value);
        Traits::store(&array[index], mask, Traits::div(vec, val));
        return *this;
    }

    __forceinline void Bind(void* bindArray, uint32_t startIndex = 0, int32_t startCount = -1)
    {
        array = (FieldType*)bindArray;
        index = startIndex;
        
        const __m256i count_vec = _mm256_set1_epi32(startCount);
        mask = _mm256_cmpgt_epi32(count_vec, FieldProxyConsts::element_indices);
    }

    __forceinline void Advance(uint32_t step)
    {
        index += step;
    }
};

// Type trait helper
template <typename T>
struct IsFieldProxy : std::false_type
{
};

template <typename T, bool MASK>
struct IsFieldProxy<FieldProxy<T, MASK>> : std::true_type
{
};