#pragma once

#include "TrinyxRingBase.h"

/**
 * TrinyxMPMCRing<T> — Bounded MPMC lock-free queue (Vyukov sequence-number pattern).
 *
 * - Multiple producers, multiple consumers — all operations use CAS.
 * - Capacity rounded to next power of 2 by Initialize().
 * - Head and Tail are on separate cache lines to prevent false sharing.
 *
 * Designed for the job system: Brain/Encoder produce, worker pool consumes.
 *
 * See TrinyxRingBase for the full queue family overview.
 */
template <typename T>
class TrinyxMPMCRing : public TrinyxRingBase<TrinyxMPMCRing<T>, T>
{
	using Base = TrinyxRingBase<TrinyxMPMCRing<T>, T>;
	using typename Base::Cell;

public:
	TrinyxMPMCRing()  = default;
	~TrinyxMPMCRing() { Base::ShutdownStorage(); }

	TrinyxMPMCRing(const TrinyxMPMCRing&)            = delete;
	TrinyxMPMCRing& operator=(const TrinyxMPMCRing&) = delete;

	/// Allocate the backing buffer. Capacity is rounded up to the next power of 2.
	bool Initialize(size_t requestedCapacity)
	{
		if (!Base::InitStorage(requestedCapacity)) return false;
		DequeuePos.store(0, std::memory_order_relaxed);
		return true;
	}

	void Shutdown() { Base::ShutdownStorage(); }

	// ------------------------------------------------------------------
	// Producer side — callable from any thread.
	// ------------------------------------------------------------------

	bool TryPush(const T& item) { return Base::MPEnqueue(item); }
	bool TryPush(T&& item)      { return Base::MPEnqueue(std::move(item)); }

	// ------------------------------------------------------------------
	// Consumer side — callable from any thread (MPMC).
	// ------------------------------------------------------------------

	/// Non-blocking dequeue. Returns false if empty.
	bool TryPop(T& item)
	{
		Cell*  cell;
		size_t pos = DequeuePos.load(std::memory_order_relaxed);
		for (;;)
		{
			cell       = &Base::Cells[pos & Base::Mask];
			size_t seq = cell->Sequence.load(std::memory_order_acquire);
			auto   diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
			if (diff == 0)
			{
				if (DequeuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
			}
			else if (diff < 0) return false; // empty
			else pos = DequeuePos.load(std::memory_order_relaxed);
		}
		item = cell->Data;
		cell->Sequence.store(pos + Base::Mask + 1, std::memory_order_release);
		return true;
	}

	bool IsEmpty() const
	{
		return Base::EnqueuePos.load(std::memory_order_relaxed) ==
		       DequeuePos.load(std::memory_order_relaxed);
	}

private:
	alignas(64) std::atomic<size_t> DequeuePos{0};
};
