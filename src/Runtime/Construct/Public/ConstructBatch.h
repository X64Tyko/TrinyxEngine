#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "Profiler.h"
#include "TickGroup.h"

// ---------------------------------------------------------------------------
// ConstructBatch — Type-erased, non-virtual tick dispatcher for Constructs.
//
// Each ConstructBatch corresponds to one tick slot (PrePhysics, PostPhysics,
// ScalarUpdate). Constructs register into the appropriate batch during
// initialization. The Brain thread calls Execute() at the correct point
// in the frame to run all registered Construct tick methods.
//
// Entries are stable-sorted by (TickGroup, OrderWithinGroup) only when the
// Dirty flag is set (registration changed since last sort). stable_sort
// preserves registration order within the same Group + Order — deterministic
// without requiring every Construct to specify explicit numbers.
// ---------------------------------------------------------------------------
class ConstructBatch
{
public:
	// Register a Construct's tick method into this batch.
	// MemFn must be a member function pointer with signature void(SimFloat).
	template <typename T, void(T::*MemFn)(SimFloat)>
	void Register(T* Object, TickGroup Group = TickGroup::Default, int16_t Order = 0)
	{
		ConstructTickEntry Entry;
		Entry.Tick.template Bind<T, MemFn>(Object);
		Entry.Group            = Group;
		Entry.OrderWithinGroup = Order;

		Entries.push_back(Entry);
		bDirty = true;
	}

	// Remove all entries for a given object pointer.
	void Deregister(void* Object)
	{
		auto It = std::remove_if(Entries.begin(), Entries.end(),
			[Object](const ConstructTickEntry& E) { return E.Tick.bindObj == Object; });

		if (It != Entries.end())
		{
			Entries.erase(It, Entries.end());
			bDirty = true;
		}
	}

	// Execute all registered tick entries in sorted order.
	void Execute(SimFloat dt)
	{
		if (Entries.empty()) return;

		TNX_ZONE_MEDIUM_NC("ConstructBatch::Execute", TNX_COLOR_LOGIC);

		if (bDirty)
		{
			std::stable_sort(Entries.begin(), Entries.end(),
				[](const ConstructTickEntry& A, const ConstructTickEntry& B)
				{
					if (A.Group != B.Group)
						return static_cast<uint8_t>(A.Group) < static_cast<uint8_t>(B.Group);
					return A.OrderWithinGroup < B.OrderWithinGroup;
				});
			bDirty = false;
		}

		for (const auto& Entry : Entries)
		{
			Entry.Tick(dt);
		}
	}

	size_t Size() const { return Entries.size(); }
	bool IsEmpty() const { return Entries.empty(); }

private:
	std::vector<ConstructTickEntry> Entries;
	bool bDirty = false;
};
