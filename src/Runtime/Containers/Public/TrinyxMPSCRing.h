#pragma once

#include "TrinyxRingBase.h"

/**
 * TrinyxMPSCRing<T> — Bounded MPSC lock-free queue (Vyukov sequence-number pattern).
 *
 * - Multiple producers may call TryPush concurrently (CAS on EnqueuePos).
 * - Exactly one consumer owns the read side via a Consumer handle.
 *   Consumer::TryPop / TryPeekAt / Size / DropFront are the only read interfaces.
 *   DequeuePos lives in the Consumer as a plain size_t — no atomics, no CAS.
 *
 * - Capacity is rounded up to the next power of 2 by Initialize().
 * - MakeConsumer() may only be called once (asserted in debug).
 *
 * Overwrite semantics (OverwritePush):
 *   When the ring is full, OverwritePush evicts the oldest entry and writes the new
 *   item in its place, so the Consumer always sees the most recent Capacity frames.
 *   OverwritePush is SP-only (single-producer). When it evicts, it increments
 *   OverflowCount; the Consumer syncs DequeuePos from this counter before each read.
 *
 * Typical usage:
 *   // Owner thread (e.g. World):
 *   TrinyxMPSCRing<NetInputFrame> InputQueue;
 *   InputQueue.Initialize(128);
 *   auto Consumer = InputQueue.MakeConsumer();   // hand to net thread
 *
 *   // Single producer thread (e.g. LogicThread):
 *   InputQueue.OverwritePush(frame);            // sliding-window semantics
 *
 *   // Net thread only (via Consumer handle):
 *   for (size_t i = 0; i < Consumer.Size(); ++i) { ... Consumer.TryPeekAt(i, f); }
 *   Consumer.DropFront(ackedCount);
 *
 * See TrinyxRingBase for the full queue family overview.
 */
template <typename T>
class TrinyxMPSCRing : public TrinyxRingBase<TrinyxMPSCRing<T>, T>
{
	using Base = TrinyxRingBase<TrinyxMPSCRing<T>, T>;
	using typename Base::Cell;

public:
	// ---------------------------------------------------------------------------
	// Consumer — single-owner read handle.
	// Obtain via MakeConsumer(). Non-copyable; move-only.
	// ---------------------------------------------------------------------------
	class Consumer
	{
		friend class TrinyxMPSCRing;

		TrinyxMPSCRing* Queue;
		mutable size_t  DequeuePos         = 0; // plain — single consumer, no contention
		mutable size_t  LastKnownOverflows  = 0; // tracks evictions from OverwritePush

		explicit Consumer(TrinyxMPSCRing* q) : Queue(q) {}

		// Advance DequeuePos to account for any overwrite-evictions the producer performed.
		// Called lazily at the start of every consumer read operation.
		void SyncOverflows() const
		{
			const size_t cur = Queue->OverflowCount.load(std::memory_order_acquire);
			if (cur > LastKnownOverflows)
			{
				DequeuePos        += cur - LastKnownOverflows;
				LastKnownOverflows = cur;
			}
		}

	public:
		Consumer(const Consumer&)            = delete;
		Consumer& operator=(const Consumer&) = delete;
		Consumer(Consumer&& o) noexcept
			: Queue(o.Queue), DequeuePos(o.DequeuePos), LastKnownOverflows(o.LastKnownOverflows)
		{ o.Queue = nullptr; }

		/// Number of items currently available to read.
		size_t Size() const
		{
			SyncOverflows();
			return Queue->EnqueuePos.load(std::memory_order_acquire) - DequeuePos;
		}

		bool IsEmpty() const { return Size() == 0; }

		/// Peek at (DequeuePos + offset) without consuming.
		/// Returns false if offset is out of range or the slot isn't committed yet.
		bool TryPeekAt(size_t offset, T& out) const
		{
			SyncOverflows();
			const size_t pos  = DequeuePos + offset;
			const Cell*  cell = &Queue->Cells[pos & Queue->Mask];
			const size_t seq  = cell->Sequence.load(std::memory_order_acquire);
			if (static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1) != 0) return false;
			out = cell->Data;
			return true;
		}

		/// Non-blocking dequeue. Returns false if empty.
		bool TryPop(T& out)
		{
			SyncOverflows();
			const size_t pos  = DequeuePos;
			Cell*        cell = &Queue->Cells[pos & Queue->Mask];
			const size_t seq  = cell->Sequence.load(std::memory_order_acquire);
			if (static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1) != 0) return false;
			out = cell->Data;
			cell->Sequence.store(pos + Queue->Mask + 1, std::memory_order_release);
			++DequeuePos;
			return true;
		}

		/// Discard the oldest n items. Stops early if the queue empties.
		void DropFront(size_t n)
		{
			T discard;
			for (size_t i = 0; i < n; ++i)
				if (!TryPop(discard)) break;
		}
	};

	// ---------------------------------------------------------------------------

	TrinyxMPSCRing()  = default;
	~TrinyxMPSCRing() { Base::ShutdownStorage(); }

	TrinyxMPSCRing(const TrinyxMPSCRing&)            = delete;
	TrinyxMPSCRing& operator=(const TrinyxMPSCRing&) = delete;

	bool Initialize(size_t requestedCapacity) { return Base::InitStorage(requestedCapacity); }
	void Shutdown()                            { Base::ShutdownStorage(); }

	/// Issue the single Consumer handle. May only be called once per instance.
	Consumer MakeConsumer()
	{
		assert(!bConsumerIssued && "TrinyxMPSCRing: only one Consumer handle may be issued");
		bConsumerIssued = true;
		return Consumer(this);
	}

	// ---------------------------------------------------------------------------
	// Producer side — callable from any thread.
	// ---------------------------------------------------------------------------

	bool TryPush(const T& item) { return Base::MPEnqueue(item); }
	bool TryPush(T&& item)      { return Base::MPEnqueue(std::move(item)); }

	/// Single-producer overwrite push. When the ring is full, the oldest entry is
	/// silently evicted so the new item always lands. The Consumer syncs via
	/// OverflowCount on its next read and fast-forwards DequeuePos accordingly.
	///
	/// SP-ONLY: must be called from a single producer thread. Multiple concurrent
	/// callers will race on the eviction and corrupt the ring.
	///
	/// Returns true if an eviction occurred (informational).
	bool OverwritePush(const T& item)
	{
		if (Base::MPEnqueue(item)) return false; // room available — no eviction

		// Ring is full. Evict the oldest slot by marking it as consumed, then retry.
		// Since this is SP, EnqueuePos is stable for the duration of this call.
		const size_t cap      = Base::Mask + 1;
		const size_t pos      = Base::EnqueuePos.load(std::memory_order_relaxed);
		Cell&        oldest   = Base::Cells[(pos - cap) & Base::Mask];
		// Mark as if TryPop had consumed it: seq = oldestPos + cap = pos
		oldest.Sequence.store(pos, std::memory_order_release);
		OverflowCount.fetch_add(1, std::memory_order_release);

		// The slot is now free — this enqueue must succeed.
		Base::MPEnqueue(item);
		return true;
	}

private:
	bool bConsumerIssued = false;
	alignas(64) std::atomic<size_t> OverflowCount{0};
};
