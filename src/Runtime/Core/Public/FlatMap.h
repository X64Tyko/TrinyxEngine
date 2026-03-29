#pragma once
#include <vector>
#include <algorithm>
#include <utility>
#include "Types.h"

template <typename TKey, typename TValue>
class FlatMap
{
public:
	using KeyType       = TKey;
	using ValueType     = TValue;
	using Entry         = std::pair<TKey, TValue>;
	using Iterator      = std::vector<Entry>::iterator;
	using ConstIterator = std::vector<Entry>::const_iterator;

	FlatMap() = default;

	// Reserve capacity upfront if you know the size
	void reserve(size_t capacity) { m_entries.reserve(capacity); }

	// Insert or update - returns iterator to inserted/updated element
	Iterator insert_or_assign(const TKey& key, const TValue& value)
	{
		auto it = lower_bound(key);
		if (it != m_entries.end() && it->first == key)
		{
			it->second = value; // Update existing
			return it;
		}
		return m_entries.insert(it, {key, value}); // Insert new
	}

	// Find - returns nullptr if not found
	FORCE_INLINE TValue* find(const TKey& key)
	{
		auto it = lower_bound(key);
		if (it != m_entries.end() && it->first == key) return &it->second;
		return nullptr;
	}

	FORCE_INLINE const TValue* find(const TKey& key) const
	{
		auto it = lower_bound(key);
		if (it != m_entries.end() && it->first == key) return &it->second;
		return nullptr;
	}

	// Operator[] for convenience (creates default value if not found)
	TValue& operator[](const TKey& key)
	{
		auto it = lower_bound(key);
		if (it != m_entries.end() && it->first == key) return it->second;
		return m_entries.insert(it, {key, TValue{}})->second;
	}

	// Find or insert a default-constructed value — returns reference to existing or new entry
	FORCE_INLINE TValue& findOrAdd(const TKey& key)
	{
		auto it = lower_bound(key);
		if (it != m_entries.end() && it->first == key) return it->second;
		return m_entries.insert(it, {key, TValue{}})->second;
	}

	// Check existence
	FORCE_INLINE bool contains(const TKey& key) const
	{
		auto it = lower_bound(key);
		return it != m_entries.end() && it->first == key;
	}

	// Erase by key - returns true if erased
	bool erase(const TKey& key)
	{
		auto it = lower_bound(key);
		if (it != m_entries.end() && it->first == key)
		{
			m_entries.erase(it);
			return true;
		}
		return false;
	}

	// Clear all entries
	void clear() { m_entries.clear(); }

	// Size and empty checks
	size_t size() const { return m_entries.size(); }
	bool empty() const { return m_entries.empty(); }

	// Iteration support
	Iterator begin() { return m_entries.begin(); }
	Iterator end() { return m_entries.end(); }
	ConstIterator begin() const { return m_entries.begin(); }
	ConstIterator end() const { return m_entries.end(); }

	// Direct access to underlying storage (for advanced use)
	const std::vector<Entry>& entries() const { return m_entries; }

	size_t count() const { return m_entries.size(); }

private:
	// Binary search for key
	FORCE_INLINE Iterator lower_bound(const TKey& key)
	{
		return std::lower_bound(m_entries.begin(), m_entries.end(), key,
								[](const Entry& e, const TKey& k) { return e.first < k; });
	}

	FORCE_INLINE ConstIterator lower_bound(const TKey& key) const
	{
		return std::lower_bound(m_entries.begin(), m_entries.end(), key,
								[](const Entry& e, const TKey& k) { return e.first < k; });
	}

private:
	std::vector<Entry> m_entries; // Kept sorted by key
};

// Specialized version for pointer values with ownership
template <typename TKey, typename TValue>
class FlatMapOwned : public FlatMap<TKey, std::unique_ptr<TValue>>
{
public:
	using Base = FlatMap<TKey, std::unique_ptr<TValue>>;

	// Convenience method to insert with ownership transfer
	TValue* insert_owned(const TKey& key, std::unique_ptr<TValue> value)
	{
		auto it = Base::insert_or_assign(key, std::move(value));
		return it->second.get();
	}

	// Get raw pointer (doesn't transfer ownership)
	TValue* get(const TKey& key)
	{
		auto* ptr = Base::find(key);
		return ptr ? ptr->get() : nullptr;
	}

	const TValue* get(const TKey& key) const
	{
		auto* ptr = Base::find(key);
		return ptr ? ptr->get() : nullptr;
	}
};
