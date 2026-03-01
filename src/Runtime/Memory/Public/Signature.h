#pragma once
#include "Types.h"
#include <bitset>
#include <functional>

// Component signature for archetype matching
// Uses bitset to track which components are present
struct Signature
{
	Signature(ComponentSignature Bits = 0)
		: Bits(Bits)
	{
	}

	ComponentSignature Bits;

	// Set a component bit
	inline void Set(ComponentTypeID TypeID)
	{
		Bits.set(TypeID);
	}

	// Clear a component bit
	inline void Clear(ComponentTypeID TypeID)
	{
		Bits.reset(TypeID);
	}

	// Check if a component is present
	inline bool Has(ComponentTypeID TypeID) const
	{
		return Bits.test(TypeID);
	}

	// Check if this signature contains all components of another signature
	inline bool Contains(const Signature& Other) const
	{
		return (Bits & Other.Bits) == Other.Bits;
	}

	// Comparison for archetype lookup
	inline bool operator==(const Signature& Other) const
	{
		return Bits == Other.Bits;
	}

	inline bool operator!=(const Signature& Other) const
	{
		return Bits != Other.Bits;
	}

	// Count number of components
	inline size_t Count() const
	{
		return Bits.count();
	}
};

// Hash specialization for std::unordered_map
namespace std
{
	template <>
	struct hash<Signature>
	{
		size_t operator()(const Signature& Sig) const noexcept
		{
			// Simple hash of bitset (can be improved if needed)
			return hash<std::bitset<MAX_COMPONENTS>>()(Sig.Bits);
		}
	};
}