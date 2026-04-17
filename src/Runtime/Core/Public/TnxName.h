#pragma once
#include <cstdint>
#include <functional>

// ---------------------------------------------------------------------------
// TnxName — hashed string identifier with owned string storage.
//
// Authoring:
//   constexpr TnxName kExplosion = TNX_NAME("explosion");  // zero runtime cost
//   TnxName dynamic("explosion");                           // hashed + stored at runtime
//   TnxName from_std(str.c_str());                         // same
//
// Comparison is always a single uint32_t compare.
// GetStr() returns the stored string in all non-stripped builds.
// In shipping builds (TNX_STRIP_NAMES) the string array is elided — struct is 4 bytes.
//
// Max name length: TNX_NAME_MAX_LEN - 1 characters (truncated silently on overflow).
// Asset names ("explosion_large", "footstep_concrete") are well within this.
// ---------------------------------------------------------------------------

static constexpr size_t TNX_NAME_MAX_LEN = 64;

struct TnxName
{
	uint32_t Value = 0;

#ifndef TNX_STRIP_NAMES
	char Str[TNX_NAME_MAX_LEN] = {};
#endif

	// Default (invalid/empty)
	constexpr TnxName() = default;

	// Internal: hash + string fill — used by TNX_NAME() macro and runtime constructors.
	constexpr TnxName(uint32_t hash, const char* str)
		: Value(hash)
#ifndef TNX_STRIP_NAMES
		, Str{}
#endif
	{
#ifndef TNX_STRIP_NAMES
		for (size_t i = 0; i < TNX_NAME_MAX_LEN - 1 && str && str[i]; ++i) Str[i] = str[i];
#endif
		(void)str;
	}

	// Runtime construction from a C-string — hashes and stores.
	explicit constexpr TnxName(const char* str)
		: TnxName(Fnv1a(str), str)
	{
	}

	bool operator==(TnxName o) const { return Value == o.Value; }
	bool operator!=(TnxName o) const { return Value != o.Value; }

	// Inline string access — use where you'd write name.GetStr() inline.
	const char* operator()() const { return GetStr(); }

	bool IsValid() const { return Value != 0; }

	// Named getters — prefer these in non-inline contexts for clarity.
	const char* GetStr() const
	{
#ifndef TNX_STRIP_NAMES
		return Str[0] ? Str : "(unnamed)";
#else
		return "";
#endif
	}

	uint32_t GetHashID() const { return Value; }

	// FNV-1a 32-bit — constexpr so TNX_NAME() is zero runtime cost.
	static constexpr uint32_t Fnv1a(const char* str)
	{
		uint32_t h = 2166136261u;
		while (str && *str) h = (h ^ static_cast<uint8_t>(*str++)) * 16777619u;
		return h;
	}
};

// Compile-time construction from a string literal.
#define TNX_NAME(str) TnxName{TnxName::Fnv1a(str), str}

// std::hash specialization — allows TnxName as unordered_map/set key.
template <>
struct std::hash<TnxName>
{
	size_t operator()(TnxName n) const noexcept
	{
		return static_cast<size_t>(n.Value);
	}
};
