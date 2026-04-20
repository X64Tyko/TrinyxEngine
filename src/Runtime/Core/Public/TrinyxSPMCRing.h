#pragma once

#include "TrinyxRingBase.h"

/**
 * TrinyxSPMCRing<T> — Bounded SPMC lock-free queue (Vyukov sequence-number pattern).
 *
 * - Exactly one producer calls TryPush (plain store on EnqueuePos — no CAS).
 * - Multiple consumers call TryPop concurrently (atomic DequeuePos + CAS).
 *
 * Typical use: one coordinator thread fans work out to a pool of workers —
 * e.g. Encoder thread producing render upload jobs that any worker can drain.
 *
 * See TrinyxRingBase for the full queue family overview.
 */
template <typename T>
class TrinyxSPMCRing : public TrinyxRingBase<TrinyxSPMCRing<T>, T>
{
	using Base = TrinyxRingBase<TrinyxSPMCRing<T>, T>;
	using typename Base::Cell;

public:
	TrinyxSPMCRing()  = default;
	~TrinyxSPMCRing() { Base::ShutdownStorage(); }

	TrinyxSPMCRing(const TrinyxSPMCRing&)            = delete;
	TrinyxSPMCRing& operator=(const TrinyxSPMCRing&) = delete;

	bool Initialize(size_t requestedCapacity)
	{
		if (!Base::InitStorage(requestedCapacity)) return false;
		DequeuePos.store(0, std::memory_order_relaxed);
		return true;
	}

	void Shutdown() { Base::ShutdownStorage(); }

	// ------------------------------------------------------------------
	// Producer side — single thread only.
	// ------------------------------------------------------------------

	bool TryPush(const T& item) { return Base::SPEnqueue(item); }
	bool TryPush(T&& item)      { return Base::SPEnqueue(std::move(item)); }

	// ------------------------------------------------------------------
	// Consumer side — callable from any thread (SPMC).
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
