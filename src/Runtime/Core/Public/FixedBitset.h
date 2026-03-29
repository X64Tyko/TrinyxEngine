#pragma once
#include <cstdint>
#include <cstring>
#ifdef _MSC_VER
#include <intrin.h>
#endif

// Fixed-size bitset backed by uint64_t words. N is rounded up to the next multiple of 64.
// Provides fast operator< (word-level comparison) and hash support — designed for use as
// a component signature in ordered and unordered containers.
template <uint32_t N>
struct FixedBitset
{
	static constexpr uint32_t WordCount = (N + 63) / 64;
	static constexpr uint32_t BitCount  = WordCount * 64;

	uint64_t Words[WordCount]{};

	constexpr FixedBitset() = default;

	// Allow construction from 0 (zero-init) to match prior std::bitset(0) usage
	constexpr FixedBitset(int)
		: Words{}
	{
	}

	void set(uint32_t bit)
	{
		Words[bit >> 6] |= (1ULL << (bit & 63));
	}

	void reset(uint32_t bit)
	{
		Words[bit >> 6] &= ~(1ULL << (bit & 63));
	}

	bool test(uint32_t bit) const
	{
		return (Words[bit >> 6] >> (bit & 63)) & 1ULL;
	}

	uint32_t count() const
	{
		uint32_t c = 0;
		for (uint32_t i = 0; i < WordCount; ++i)
#ifdef _MSC_VER
		c += static_cast<uint32_t>(__popcnt64(Words[i]));
#else
			c += __builtin_popcountll(Words[i]);
#endif
		return c;
	}

	// Bitwise AND
	FixedBitset operator&(const FixedBitset& other) const
	{
		FixedBitset result;
		for (uint32_t i = 0; i < WordCount; ++i) result.Words[i] = Words[i] & other.Words[i];
		return result;
	}

	// Bitwise OR assignment
	FixedBitset& operator|=(const FixedBitset& other)
	{
		for (uint32_t i = 0; i < WordCount; ++i) Words[i] |= other.Words[i];
		return *this;
	}

	bool operator==(const FixedBitset& other) const
	{
		return std::memcmp(Words, other.Words, sizeof(Words)) == 0;
	}

	bool operator!=(const FixedBitset& other) const
	{
		return !(*this == other);
	}

	// Lexicographic comparison from high word to low — first differing word determines order.
	bool operator<(const FixedBitset& other) const
	{
		for (int i = WordCount - 1; i >= 0; --i)
		{
			if (Words[i] != other.Words[i]) return Words[i] < other.Words[i];
		}
		return false;
	}

	// Fast hash — FNV-1a over the word array
	size_t hash() const
	{
		size_t h = 14695981039346656037ULL;
		for (uint32_t i = 0; i < WordCount; ++i)
		{
			h ^= Words[i];
			h *= 1099511628211ULL;
		}
		return h;
	}
};