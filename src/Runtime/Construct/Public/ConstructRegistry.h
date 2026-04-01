#pragma once

#include <vector>
#include <memory>
#include <cstdint>

class World;

// ---------------------------------------------------------------------------
// ConstructRegistry — Type-erased registry of all live Constructs.
//
// Tracks Construct lifetimes, assigns IDs, and handles deferred destruction.
// World owns the ConstructRegistry. LogicThread calls
// ProcessDeferredDestructions() at the top of each frame alongside the
// entity equivalent.
//
// Usage:
//   auto* player = constructs->Create<PlayerConstruct>(world);
//   constructs->Destroy(player->GetConstructID());
// ---------------------------------------------------------------------------
class ConstructRegistry
{
public:
	ConstructRegistry() = default;
	~ConstructRegistry() { DestroyAll(); }

	ConstructRegistry(const ConstructRegistry&)            = delete;
	ConstructRegistry& operator=(const ConstructRegistry&) = delete;

	template <typename T>
	T* Create(World* InWorld)
	{
		auto typed = std::make_unique<TypedStorage<T>>();
		T* raw     = &typed->Value;

		uint32_t id = NextID++;
		raw->SetConstructID(id);
		raw->Initialize(InWorld);

		Entries.push_back({id, raw, std::move(typed)});
		return raw;
	}

	void Destroy(uint32_t id)
	{
		PendingDestructions.push_back(id);
	}

	void ProcessDeferredDestructions()
	{
		if (PendingDestructions.empty()) return;

		for (uint32_t id : PendingDestructions)
		{
			for (size_t i = 0; i < Entries.size(); ++i)
			{
				if (Entries[i].ID == id)
				{
					if (i != Entries.size() - 1) Entries[i] = std::move(Entries.back());
					Entries.pop_back();
					break;
				}
			}
		}
		PendingDestructions.clear();
	}

	void DestroyAll()
	{
		Entries.clear();
		PendingDestructions.clear();
	}

	uint32_t GetCount() const { return static_cast<uint32_t>(Entries.size()); }

	template <typename Func>
	void ForEach(Func&& fn)
	{
		for (auto& entry : Entries) fn(entry.Ptr, entry.ID);
	}

private:
	struct StorageBase
	{
		virtual ~StorageBase() = default;
	};

	template <typename T>
	struct TypedStorage : StorageBase
	{
		T Value;
	};

	struct Entry
	{
		uint32_t ID = 0;
		void* Ptr   = nullptr;
		std::unique_ptr<StorageBase> Storage;
	};

	std::vector<Entry> Entries;
	std::vector<uint32_t> PendingDestructions;
	uint32_t NextID = 1;
};