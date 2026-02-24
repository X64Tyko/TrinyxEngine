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
    // Non-temporal store (bypasses cache, for write-only temporal data)
    static __forceinline void stream(float* ptr, [[maybe_unused]] __m256i mask, VecType val)
    {
        if constexpr (MASK) { _mm256_maskstore_ps(ptr, mask, val); }  // No masked stream, fall back
        else { _mm256_stream_ps(ptr, val); }
    }
    static __forceinline VecType set1(float val) { return _mm256_set1_ps(val); }
    static __forceinline VecType add(VecType a, VecType b) { return _mm256_add_ps(a, b); }
    static __forceinline VecType sub(VecType a, VecType b) { return _mm256_sub_ps(a, b); }
    static __forceinline VecType mul(VecType a, VecType b) { return _mm256_mul_ps(a, b); }
    static __forceinline VecType div(VecType a, VecType b) { return _mm256_div_ps(a, b); }
    static __forceinline VecType GT(VecType a, VecType b) { return _mm256_cmp_ps(a, b, _CMP_GT_OQ); }
    static __forceinline VecType LT(VecType a, VecType b) { return _mm256_cmp_ps(a, b, _CMP_LT_OQ); }
    static __forceinline VecType Blend(VecType a, VecType b) { return _mm256_blendv_ps(a, b, GT(a,b)); }
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
    // Non-temporal store (bypasses cache, for write-only temporal data)
    static __forceinline void stream(int32_t* ptr, [[maybe_unused]] __m256i mask, VecType val)
    {
        if constexpr (MASK) { _mm256_maskstore_epi32(ptr, mask, val); }  // No masked stream, fall back
        else { _mm256_stream_si256((__m256i*)ptr, val); }
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
    static __forceinline VecType GT(VecType a, VecType b) { return _mm256_cmpgt_epi32(a, b); }
    static __forceinline VecType LT(VecType a, VecType b) { return _mm256_cmpgt_epi32(b, a); }
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
    // Non-temporal store (bypasses cache, for write-only temporal data)
    static __forceinline void stream(uint32_t* ptr, [[maybe_unused]] __m256i mask, VecType val)
    {
        if constexpr (MASK) { _mm256_maskstore_epi32((int32_t*)ptr, mask, val); }  // No masked stream, fall back
        else { _mm256_stream_si256((__m256i*)ptr, val); }
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
    static __forceinline VecType GT(VecType a, VecType b) { return _mm256_cmpgt_epi32(a, b); }
    static __forceinline VecType LT(VecType a, VecType b) { return _mm256_cmpgt_epi32(b, a); }
};

template <typename T, typename FIELDTYPE, typename VECTYPE>
concept ProxyType = std::is_same_v<std::remove_cvref_t<T>, FIELDTYPE> || std::is_same_v<std::remove_cvref_t<T>, VECTYPE> || SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value;

// Helper: Proxy for individual field access with SIMD
template <typename FieldType, bool MASK>
struct FieldProxy
{
    using Traits = SIMDTraits<FieldType, MASK>;

    FieldType* __restrict ReadArray = nullptr;   // Frame T (current simulation state)
    FieldType* __restrict WriteArray = nullptr;  // Frame T+1 (next simulation state)
    uint32_t index;
    __m256i mask = _mm256_set1_epi64x(-1);  // Only used when MASK = true

    // Select: _mm256 blend. creates a mask by comparing current value > ComVal and either selecting TrueVal if true and FalseVal if false
    template <ProxyType<FieldType, typename Traits::VecType> COMP, ProxyType<FieldType, typename Traits::VecType> FVAL, ProxyType<FieldType, typename Traits::VecType> TVAL>
    __forceinline decltype(auto) Select(COMP&& CompVal, FVAL&& FalseVal, TVAL&& TrueVal)
    {
        typename Traits::VecType Val = Traits::load(&WriteArray[index]);
        
        typename Traits::VecType cmpV;
        if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<COMP>>::value)
            cmpV = Traits::load(&CompVal.WriteArray[CompVal.index]);
        else if constexpr (std::is_same_v<COMP, std::remove_cvref_t<FieldType>>)
            cmpV = Traits::set1(CompVal);
        else
            cmpV = CompVal;
        
        auto CmpMask = Traits::GT(Val, cmpV);

        typename Traits::VecType falseV;
        if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<FVAL>>::value)
            falseV = Traits::load(&FalseVal.WriteArray[FalseVal.index]);
        else if constexpr (std::is_same_v<FVAL, std::remove_cvref_t<FieldType>>)
            falseV = Traits::set1(FalseVal);
        else
            falseV = FalseVal;

        typename Traits::VecType trueV;
        if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<TVAL>>::value)
            trueV = Traits::load(&TrueVal.WriteArray[TrueVal.index]);
        else if constexpr (std::is_same_v<TVAL, std::remove_cvref_t<FieldType>>)
            trueV = Traits::set1(TrueVal);
        else
            trueV = TrueVal;

        // Blend: select trueV where mask is set, otherwise falseV
        typename Traits::VecType result = _mm256_blendv_ps(falseV, trueV, CmpMask);

        Traits::store(&WriteArray[index], mask, result);
        return *this;
    }

    template <ProxyType<FieldType, typename Traits::VecType> T>
    __forceinline decltype(auto) operator=(T&& value)
    {
        typename Traits::VecType VecVal;
        if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value)
            VecVal = Traits::load(&value.WriteArray[value.index]);
        else if constexpr (std::is_same_v<T, std::remove_cvref_t<FieldType>>)
            VecVal = Traits::set1(value);
        else
            VecVal = value;
        
        Traits::store(&WriteArray[index], mask, VecVal);
        return *this;
    }

    template <ProxyType<FieldType, typename Traits::VecType> T>
    __forceinline decltype(auto) operator+=(T&& value)
    {
        typename Traits::VecType VecVal;
        if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value)
            VecVal = Traits::load(&value.WriteArray[value.index]);
        else if constexpr (std::is_same_v<T, std::remove_cvref_t<FieldType>>)
            VecVal = Traits::set1(value);
        else
            VecVal = value;

        Traits::store(&WriteArray[index], mask, Traits::add(Traits::load(&WriteArray[index]), VecVal));
        return *this;
    }

    template <ProxyType<FieldType, typename Traits::VecType> T>
    __forceinline decltype(auto) operator-=(T&& value)
    {
        typename Traits::VecType VecVal;
        if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value)
            VecVal = Traits::load(&value.WriteArray[value.index]);
        else if constexpr (std::is_same_v<T, std::remove_cvref_t<FieldType>>)
            VecVal = Traits::set1(value);
        else
            VecVal = value;

        Traits::store(&WriteArray[index], mask, Traits::sub(Traits::load(&WriteArray[index]), VecVal));
        return *this;
    }

    template <ProxyType<FieldType, typename Traits::VecType> T>
    __forceinline decltype(auto) operator*=(T&& value)
    {
        typename Traits::VecType VecVal;
        if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value)
            VecVal = Traits::load(&value.WriteArray[value.index]);
        else if constexpr (std::is_same_v<T, std::remove_cvref_t<FieldType>>)
            VecVal = Traits::set1(value);
        else
            VecVal = value;

        Traits::store(&WriteArray[index], mask, Traits::mul(Traits::load(&WriteArray[index]), VecVal));
        return *this;
    }

    template <ProxyType<FieldType, typename Traits::VecType> T>
    __forceinline decltype(auto) operator/=(T&& value)
    {
        typename Traits::VecType VecVal;
        if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<T>>::value)
            VecVal = Traits::load(&value.WriteArray[value.index]);
        else if constexpr (std::is_same_v<T, std::remove_cvref_t<FieldType>>)
            VecVal = Traits::set1(value);
        else
            VecVal = value;

        Traits::store(&WriteArray[index], mask, Traits::div(Traits::load(&WriteArray[index]), VecVal));
        return *this;
    }

    // Bind: load from read frame T, immediately stream to frame T+1 (non-temporal), cache T+1 for modifications
    __forceinline void Bind(void* readArray, void* writeArray, uint32_t startIndex = 0, int32_t startCount = -1)
    {
        ReadArray = (FieldType*)readArray;
        WriteArray = (FieldType*)writeArray;
        index = startIndex;

        const __m256i count_vec = _mm256_set1_epi32(startCount);
        mask = _mm256_cmpgt_epi32(count_vec, FieldProxyConsts::element_indices);

        // Immediately store to frame T+1
        Traits::store(&WriteArray[index], mask, Traits::load(&ReadArray[index]));
    }

    // Advance: stream cached vector back, advance index, load from T and stream to T+1
    __forceinline void Advance(uint32_t step)
    {
        index += step;
        
        Traits::store(&WriteArray[index], mask, Traits::load(&ReadArray[index]));
    }
    
    // FRIEND OPERATORS
    template <ProxyType<FieldType, typename Traits::VecType> L, ProxyType<FieldType, typename Traits::VecType> R>
    __forceinline friend decltype(auto) operator*(L&& LHS, R&& RHS)
    {
        typename Traits::VecType LVal;
        if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<L>>::value)
            LVal = Traits::load(&LHS.WriteArray[LHS.index]);
        else if constexpr (std::is_same_v<L, std::remove_cvref_t<FieldType>>)
            LVal = Traits::set1(LHS);
        else
            LVal = LHS;
        
        typename Traits::VecType RVal;
        if constexpr (SchemaValidation::IsFieldProxy<std::remove_cvref_t<R>>::value)
            RVal = Traits::load(&RHS.WriteArray[RHS.index]);
        else if constexpr (std::is_same_v<R, std::remove_cvref_t<FieldType>>)
            RVal = Traits::set1(RHS);
        else
            RVal = RHS;
        
        return Traits::mul(LVal, RVal);
    }
};