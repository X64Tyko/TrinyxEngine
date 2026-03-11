#include "TrinyxJobs.h"
#include "EngineConfig.h"
#include "Logger.h"
#include "Profiler.h"
#include "ThreadPinning.h"

#include <cassert>
#include <thread>
#include <vector>
#include <xmmintrin.h>

struct EngineConfig;

namespace TrinyxJobs
{
	// ---- Internal state --------------------------------------------------

	static constexpr uint32_t kQueueCount = static_cast<uint32_t>(Queue::COUNT);

	// One ring buffer per queue
	static TrinyxRingBuffer<Job> s_Queues[kQueueCount];

	// Worker threads
	static std::vector<std::thread> s_Workers;
	static std::atomic<bool> s_Running{false};
	static uint32_t s_WorkerCount = 0;

	// Thread-local identity (1-based for workers, 0 = coordinator/non-worker)
	static thread_local uint32_t t_ThreadIndex = 0;

	// Wake signal — incremented on every SubmitJob, workers wait() on it.
	// Uses std::atomic::wait/notify (C++20): futex on Linux, WaitOnAddress on Windows.
	// Workers block until the value changes, meaning zero CPU when idle.
	static std::atomic<uint32_t> s_WakeSignal{0};
	
	// TODO: make this configurable through ini
	static uint64_t MaxSpins = 4000;

	// ---- Internal helpers ------------------------------------------------

	/// Try to pop and execute one job from any queue.
	/// Returns true if work was found. Used by workers and WaitForCounter.
	static bool StealAndExecute(const std::vector<uint8_t>& queues)
	{
		// Priority order: Physics > Render > General
		// This ensures physics work (the tightest budget) drains fastest.
		Job job;
		for (auto& queueID : queues)
		{
			if (s_Queues[queueID].TryPop(job))
			{
				job.EntryPoint(job.Payload, t_ThreadIndex);
				if (job.Counter)
				{
					job.Counter->fetch_sub(1, std::memory_order_release);
					job.Counter->notify_one();
					return true;
				}
			}
		}
		return false;
	}

	/// Worker thread entry point.
	static void WorkerMain(uint32_t workerIndex, Queue affinity)
	{
		// create queue inices for worker affinity
		std::vector<uint8_t> queues;
		for (uint8_t id = 0; id < kQueueCount; ++id)
		{
			if ((static_cast<uint8_t>(affinity) & (1 << id)) == (1 << id)) queues.push_back(id);
		}

		t_ThreadIndex = workerIndex;

		uint64_t spinCount = 0;
		while (s_Running.load(std::memory_order_relaxed))
		{
			TNX_ZONE_FINE_NC("Worker_Tick", TNX_COLOR_WORKER);

			if (StealAndExecute(queues))
			{
				spinCount = 0;
				continue;
			} // Got work — immediately try again, no delay.

			if (++spinCount < MaxSpins) { _mm_pause(); continue; } // Spin for a bit to see if a job comes in.

			// No work available. Snapshot the wake signal, then check queues
			// one more time before blocking (avoids missed-wake race).
			uint32_t snapshot = s_WakeSignal.load(std::memory_order_acquire);

			if (StealAndExecute(queues)) continue; // Work arrived between the first check and snapshot.

			// Block until SubmitJob bumps the signal (futex / WaitOnAddress).
			// Wakes in ~1-2μs on Linux, near-zero CPU while idle.
			if (s_Running.load(std::memory_order_relaxed)) s_WakeSignal.wait(snapshot, std::memory_order_relaxed);
		}

		// Drain remaining jobs before exiting
		while (StealAndExecute(queues))
		{
		}
	}

	// ---- Public API ------------------------------------------------------

	bool Initialize(const EngineConfig* config)
	{
		assert(config);

		const size_t queueCapacity = static_cast<size_t>(config->JobCacheSize);

		for (uint32_t q = 0; q < kQueueCount; ++q)
		{
			if (!s_Queues[q].Initialize(queueCapacity))
			{
				LOG_ERROR("[Jobs] Failed to allocate queue");
				return false;
			}
		}

		s_WakeSignal.store(0, std::memory_order_relaxed);

		// Spawn workers — count determined by ThreadPinning topology scan
		s_WorkerCount = TrinyxThreading::GetWorkerThreadCapacity();
		if (s_WorkerCount == 0)
		{
			LOG_ERROR("[Jobs] No cores available for workers");
			return false;
		}

		s_Running.store(true, std::memory_order_release);
		s_Workers.reserve(s_WorkerCount);

		uint8_t PhysicsDedicated = s_WorkerCount * 0.25;
		for (uint32_t i = 0; i < s_WorkerCount; ++i)
		{
			// Worker indices are 1-based (0 = coordinator/non-worker)
			s_Workers.emplace_back(WorkerMain, i + 1, i < PhysicsDedicated ? Queue::Physics : Queue::All);
			TrinyxThreading::PinThread(s_Workers.back());
		}

		LOG_INFO_F("[Jobs] Initialized: %u workers, %zu-slot queues (Phys/Rend/Genl)",
				   s_WorkerCount, queueCapacity);
		return true;
	}

	void Shutdown()
	{
		if (!s_Running.load(std::memory_order_relaxed)) return;

		s_Running.store(false, std::memory_order_release);

		// Wake all workers so they see s_Running == false and exit
		s_WakeSignal.fetch_add(1, std::memory_order_release);
		s_WakeSignal.notify_all();

		for (auto& w : s_Workers)
		{
			if (w.joinable()) w.join();
		}
		s_Workers.clear();
		s_WorkerCount = 0;

		for (uint32_t q = 0; q < kQueueCount; ++q) s_Queues[q].Shutdown();

		LOG_INFO("[Jobs] Shutdown complete");
	}

	void SubmitJob(const Job& job, Queue queue)
	{
		uint32_t idx = std::countr_zero(static_cast<uint32_t>(queue));
		assert(idx < kQueueCount);

		bool pushed = s_Queues[idx].TryPush(job);

		// If the primary queue is full, spill to General.
		// If General is also full, we've exceeded JobCacheSize — assert.
		if (!pushed && queue != Queue::General)
		{
			pushed = s_Queues[std::countr_zero(static_cast<uint8_t>(Queue::General))].TryPush(job);
			LOG_ERROR("[Jobs] Queue full");
		}

		assert(pushed && "Job queues full — increase JobCacheSize in config");
		(void)pushed;

		// Wake one sleeping worker
		s_WakeSignal.fetch_add(1, std::memory_order_release);
		s_WakeSignal.notify_one();
	}

	void WaitForCounter(JobCounter* counter, Queue affinity)
	{
		// create queue inices for worker affinity
		std::vector<uint8_t> queues;
		for (uint8_t id = 0; id < kQueueCount; ++id)
		{
			if ((static_cast<uint8_t>(affinity) & (1 << id)) == (1 << id)) queues.push_back(id);
		}

		TNX_ZONE_N("Jobs_WaitForCounter");

		// The calling thread becomes a worker while waiting.
		// This is how coordinators (Brain/Encoder) contribute to throughput
		// instead of sitting idle.
		while (counter->Value.load(std::memory_order_acquire) > 0)
		{
			if (StealAndExecute(queues)) continue;

			// No work to steal — wait for a signal rather than spin.
			// Short-lived: the jobs we're waiting on will wake us when they
			// complete and produce follow-up work, or when new jobs arrive.
			uint32_t snapshot = counter->Value.load(std::memory_order_acquire);

			// Double-check after snapshot to avoid missed wake
			if (counter->Value.load(std::memory_order_acquire) == 0) break;
			if (StealAndExecute(queues)) continue;

			counter->Value.wait(snapshot, std::memory_order_relaxed);
		}
	}

	uint32_t GetWorkerCount()
	{
		return s_WorkerCount;
	}

	uint32_t GetCurrentThreadIndex()
	{
		return t_ThreadIndex;
	}
}
