#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <new>
#include <utility>

/**
 * TrinyxRingBase<Derived, T> — CRTP base for Vyukov sequence-number ring queues.
 *
 * Provides:
 *   - Cell layout (sequence atomic + data)
 *   - Power-of-2 backing storage (InitStorage / ShutdownStorage)
 *   - EnqueuePos (producers always CAS or plain-store into this)
 *   - Capacity()
 *
 * Derived classes are responsible for:
 *   - Calling InitStorage / ShutdownStorage in their own Initialize / Shutdown
 *   - Implementing TryPush (MP → CAS; SP → plain store)
 *   - Implementing TryPop / Consumer handle (MC → atomic DequeuePos+CAS; SC → plain size_t)
 *
 * Queue families:
 *   TrinyxMPMCRing<T>  — Multiple producers, multiple consumers
 *   TrinyxMPSCRing<T>  — Multiple producers, single Consumer handle
 *   TrinyxSPMCRing<T>  — Single producer, multiple consumers        (future)
 *   TrinyxSPSCQueue<T> — Single producer, single consumer           (future, no seq atomics)
 */
template <typename Derived, typename T>
class TrinyxRingBase
{
protected:
	struct Cell
	{
		std::atomic<size_t> Sequence{0};
		T                   Data;
	};

	/// Allocate and initialise the backing buffer. Capacity rounded to next power of 2.
	bool InitStorage(size_t requestedCapacity)
	{
		assert(requestedCapacity > 0);
		size_t cap = 1;
		while (cap < requestedCapacity) cap <<= 1;

		Mask  = cap - 1;
		Cells = new(std::nothrow) Cell[cap];
		if (!Cells) return false;

		for (size_t i = 0; i < cap; ++i) Cells[i].Sequence.store(i, std::memory_order_relaxed);
		EnqueuePos.store(0, std::memory_order_relaxed);
		return true;
	}

	void ShutdownStorage()
	{
		delete[] Cells;
		Cells = nullptr;
		Mask  = 0;
	}

	/// Vyukov MP enqueue — CAS on EnqueuePos. Call from TryPush in MP-producer derived classes.
	template <typename U>
	bool MPEnqueue(U&& item)
	{
		Cell*  cell;
		size_t pos = EnqueuePos.load(std::memory_order_relaxed);
		for (;;)
		{
			cell       = &Cells[pos & Mask];
			size_t seq = cell->Sequence.load(std::memory_order_acquire);
			auto   diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
			if (diff == 0)
			{
				if (EnqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
			}
			else if (diff < 0) return false; // full
			else pos = EnqueuePos.load(std::memory_order_relaxed);
		}
		cell->Data = std::forward<U>(item);
		cell->Sequence.store(pos + 1, std::memory_order_release);
		return true;
	}

	/// Vyukov SP enqueue — plain store on EnqueuePos (no CAS; only one producer).
	/// Call from TryPush in SP-producer derived classes.
	template <typename U>
	bool SPEnqueue(U&& item)
	{
		const size_t pos  = EnqueuePos.load(std::memory_order_relaxed);
		Cell*        cell = &Cells[pos & Mask];
		const size_t seq  = cell->Sequence.load(std::memory_order_acquire);
		const auto   diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
		if (diff < 0) return false; // full
		cell->Data = std::forward<U>(item);
		cell->Sequence.store(pos + 1, std::memory_order_release);
		EnqueuePos.store(pos + 1, std::memory_order_relaxed);
		return true;
	}

	alignas(64) std::atomic<size_t> EnqueuePos{0};

	Cell*  Cells = nullptr;
	size_t Mask  = 0;

public:
	size_t Capacity() const { return Mask + 1; }

	TrinyxRingBase()                                 = default;
	TrinyxRingBase(const TrinyxRingBase&)            = delete;
	TrinyxRingBase& operator=(const TrinyxRingBase&) = delete;
};
