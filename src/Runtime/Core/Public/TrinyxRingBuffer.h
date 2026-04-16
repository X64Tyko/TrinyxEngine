#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <new>

/**
 * TrinyxRingBuffer<T> — Bounded MPMC lock-free queue (Vyukov pattern).
 *
 * - Capacity must be a power of 2 (enforced by Initialize).
 * - Head and Tail are on separate cache lines to prevent false sharing.
 * - Each cell carries a sequence number for lock-free coordination.
 * - TryPush/TryPop are wait-free in the uncontended case, lock-free under contention.
 *
 * Designed for the job system: Brain/Encoder produce, worker pool consumes.
 */
template <typename T>
class TrinyxRingBuffer
{
public:
	TrinyxRingBuffer() = default;
	~TrinyxRingBuffer() { Shutdown(); }

	TrinyxRingBuffer(const TrinyxRingBuffer&)            = delete;
	TrinyxRingBuffer& operator=(const TrinyxRingBuffer&) = delete;

	/// Allocate the backing buffer. Capacity is rounded up to the next power of 2.
	bool Initialize(size_t requestedCapacity)
	{
		assert(requestedCapacity > 0);

		// Round up to power of 2
		size_t cap = 1;
		while (cap < requestedCapacity) cap <<= 1;

		Mask  = cap - 1;
		Cells = new(std::nothrow) Cell[cap];
		if (!Cells) return false;

		for (size_t i = 0; i < cap; ++i) Cells[i].Sequence.store(i, std::memory_order_relaxed);

		EnqueuePos.store(0, std::memory_order_relaxed);
		DequeuePos.store(0, std::memory_order_relaxed);
		return true;
	}

	void Shutdown()
	{
		delete[] Cells;
		Cells = nullptr;
		Mask  = 0;
	}

	/// Non-blocking enqueue. Returns false if the queue is full.
	bool TryPush(const T& item)
	{
		Cell* cell;
		size_t pos = EnqueuePos.load(std::memory_order_relaxed);
		for (;;)
		{
			cell       = &Cells[pos & Mask];
			size_t seq = cell->Sequence.load(std::memory_order_acquire);
			auto diff  = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
			if (diff == 0)
			{
				if (EnqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
			}
			else if (diff < 0)
			{
				return false; // full
			}
			else
			{
				pos = EnqueuePos.load(std::memory_order_relaxed); // retry
			}
		}
		cell->Data = item;
		cell->Sequence.store(pos + 1, std::memory_order_release);
		return true;
	}

	/// Non-blocking move-enqueue. Returns false if the queue is full.
	bool TryPush(T&& item)
	{
		Cell* cell;
		size_t pos = EnqueuePos.load(std::memory_order_relaxed);
		for (;;)
		{
			cell       = &Cells[pos & Mask];
			size_t seq = cell->Sequence.load(std::memory_order_acquire);
			auto diff  = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
			if (diff == 0)
			{
				if (EnqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
			}
			else if (diff < 0)
			{
				return false; // full
			}
			else
			{
				pos = EnqueuePos.load(std::memory_order_relaxed); // retry
			}
		}
		cell->Data = std::move(item);
		cell->Sequence.store(pos + 1, std::memory_order_release);
		return true;
	}

	/// Non-blocking dequeue. Returns false if the queue is empty.
	bool TryPop(T& item)
	{
		Cell* cell;
		size_t pos = DequeuePos.load(std::memory_order_relaxed);
		for (;;)
		{
			cell       = &Cells[pos & Mask];
			size_t seq = cell->Sequence.load(std::memory_order_acquire);
			auto diff  = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
			if (diff == 0)
			{
				if (DequeuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
			}
			else if (diff < 0)
			{
				return false; // empty
			}
			else
			{
				pos = DequeuePos.load(std::memory_order_relaxed); // retry
			}
		}
		item = cell->Data;
		cell->Sequence.store(pos + Mask + 1, std::memory_order_release);
		return true;
	}

	size_t Capacity() const { return Mask + 1; }

	bool IsEmpty() const
	{
		size_t head = EnqueuePos.load(std::memory_order_relaxed);
		size_t tail = DequeuePos.load(std::memory_order_relaxed);
		return head == tail;
	}

private:
	struct Cell
	{
		std::atomic<size_t> Sequence{0};
		T Data;
	};

	alignas(64) std::atomic<size_t> EnqueuePos{0};
	alignas(64) std::atomic<size_t> DequeuePos{0};

	Cell* Cells = nullptr;
	size_t Mask = 0;
};
