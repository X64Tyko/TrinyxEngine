#pragma once
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>
#include <cassert>

#include "Types.h"

template <
	uint32_t MaxKey, // e.g. 1u << 24
	typename TValue,
	uint32_t EntriesPerPage = 4096, // must be power of two
	TValue InvalidValue = TValue{}  // override for sentinel values (e.g. 0xFFFFFFFF)
>
class PagedMap
{
	static_assert(std::is_trivially_copyable_v<TValue>, "PagedMap TValue must be trivially copyable");
	static_assert((EntriesPerPage & (EntriesPerPage - 1)) == 0, "EntriesPerPage must be power of two");
	static_assert(MaxKey % EntriesPerPage == 0, "MaxKey must be divisible by EntriesPerPage");

public:
	using Key   = uint32_t;
	using Value = TValue;

	PagedMap()
	{
		const uint32_t pageCount = MaxKey / EntriesPerPage;
		m_pages.resize(pageCount, nullptr);
	}

	~PagedMap() { clear_all(); }

	PagedMap(const PagedMap&)            = delete;
	PagedMap& operator=(const PagedMap&) = delete;
	
	FORCE_INLINE Value operator[](Key key) const { return get(key); }
	FORCE_INLINE Value* operator[](Key key) { return try_get_ptr(key); }

	FORCE_INLINE Value get(Key key) const
	{
		if (key >= MaxKey) return InvalidValue;
		const uint32_t p = page_index(key);
		const uint32_t o = page_offset(key);
		Value* page      = m_pages[p];
		return page ? page[o] : InvalidValue;
	}

	FORCE_INLINE Value* try_get_ptr(Key key)
	{
		if (key >= MaxKey) return nullptr;
		const uint32_t p = page_index(key);
		Value* page      = m_pages[p];
		if (!page) return nullptr;
		return &page[page_offset(key)];
	}

	FORCE_INLINE const Value* try_get_ptr(Key key) const
	{
		if (key >= MaxKey) return nullptr;
		const uint32_t p = page_index(key);
		Value* page      = m_pages[p];
		if (!page) return nullptr;
		return &page[page_offset(key)];
	}

	FORCE_INLINE void set(Key key, Value v)
	{
		assert(key < MaxKey);
		const uint32_t p = page_index(key);
		Value* page      = m_pages[p];
		if (!page) page = allocate_page(p);
		page[page_offset(key)] = v;
	}

	FORCE_INLINE void erase(Key key)
	{
		if (key >= MaxKey) return;
		const uint32_t p = page_index(key);
		Value* page      = m_pages[p];
		if (!page) return;
		page[page_offset(key)] = InvalidValue;
		// Optional: track emptiness and free page when all entries are invalid.
	}

	void clear_all()
	{
		for (Value*& page : m_pages)
		{
			if (page)
			{
				delete[] page;
				page = nullptr;
			}
		}
	}

	uint32_t page_count() const { return (uint32_t)m_pages.size(); }

private:
	static constexpr uint32_t kPageShift = []
	{
		uint32_t s = 0;
		uint32_t n = EntriesPerPage;
		while ((n >>= 1) != 0) ++s;
		return s;
	}();
	static constexpr uint32_t kPageMask = EntriesPerPage - 1;

	FORCE_INLINE static uint32_t page_index(Key k) { return k >> kPageShift; }
	FORCE_INLINE static uint32_t page_offset(Key k) { return k & kPageMask; }

	Value* allocate_page(uint32_t pageIndex)
	{
		Value* page = new Value[EntriesPerPage];
		// Fill with sentinel invalid value
		if constexpr (std::is_integral_v<Value> || std::is_enum_v<Value>)
		{
			for (uint32_t i = 0; i < EntriesPerPage; ++i) page[i] = InvalidValue;
		}
		else
		{
			std::memset(page, 0, sizeof(Value) * EntriesPerPage); // only safe if kInvalidValue==zero-ish POD
		}
		m_pages[pageIndex] = page;
		return page;
	}

private:
	std::vector<Value*> m_pages;
};
