#include "TrinyxJobs.h"
#include "EngineConfig.h"
#include "Logger.h"
#include "Profiler.h"
#include "ThreadPinning.h"

#include <cassert>
#include <thread>
#include <vector>

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

	// ---- Internal helpers ------------------------------------------------

	/// Try to pop and execute one job from any queue.
	/// Returns true if work was found. Used by workers and WaitForCounter.
	static bool StealAndExecute()
	{
		// Priority order: Physics > Render > General
		// This ensures physics work (the tightest budget) drains fastest.
		Job job;
		for (uint32_t q = 0; q < kQueueCount; ++q)
		{
			if (s_Queues[q].TryPop(job))
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
	static void WorkerMain(uint32_t workerIndex)
	{
		t_ThreadIndex = workerIndex;

		while (s_Running.load(std::memory_order_relaxed))
		{
			TNX_ZONE_NC("Worker_Tick", TNX_COLOR_WORKER);

			if (StealAndExecute()) continue; // Got work — immediately try again, no delay.

			// No work available. Snapshot the wake signal, then check queues
			// one more time before blocking (avoids missed-wake race).
			uint32_t snapshot = s_WakeSignal.load(std::memory_order_acquire);

			if (StealAndExecute()) continue; // Work arrived between the first check and snapshot.

			// Block until SubmitJob bumps the signal (futex / WaitOnAddress).
			// Wakes in ~1-2μs on Linux, near-zero CPU while idle.
			if (s_Running.load(std::memory_order_relaxed)) s_WakeSignal.wait(snapshot, std::memory_order_relaxed);
		}

		// Drain remaining jobs before exiting
		while (StealAndExecute())
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

		for (uint32_t i = 0; i < s_WorkerCount; ++i)
		{
			// Worker indices are 1-based (0 = coordinator/non-worker)
			s_Workers.emplace_back(WorkerMain, i + 1);
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
		auto idx = static_cast<uint32_t>(queue);
		assert(idx < kQueueCount);

		bool pushed = s_Queues[idx].TryPush(job);

		// If the primary queue is full, spill to General.
		// If General is also full, we've exceeded JobCacheSize — assert.
		if (!pushed && queue != Queue::General) pushed = s_Queues[static_cast<uint32_t>(Queue::General)].TryPush(job);

		assert(pushed && "Job queues full — increase JobCacheSize in config");
		(void)pushed;

		// Wake one sleeping worker
		s_WakeSignal.fetch_add(1, std::memory_order_release);
		s_WakeSignal.notify_one();
	}

	void WaitForCounter(JobCounter* counter)
	{
		TNX_ZONE_N("Jobs_WaitForCounter");

		// The calling thread becomes a worker while waiting.
		// This is how coordinators (Brain/Encoder) contribute to throughput
		// instead of sitting idle.
		while (counter->Value.load(std::memory_order_acquire) > 0)
		{
			if (StealAndExecute()) continue;

			// No work to steal — wait for a signal rather than spin.
			// Short-lived: the jobs we're waiting on will wake us when they
			// complete and produce follow-up work, or when new jobs arrive.
			uint32_t snapshot = counter->Value.load(std::memory_order_acquire);

			// Double-check after snapshot to avoid missed wake
			if (counter->Value.load(std::memory_order_acquire) == 0) break;
			if (StealAndExecute()) continue;

			counter->Value.wait(snapshot, std::memory_order_relaxed);
		}
	}

	void RenderWaitForCounter(JobCounter* counter)
	{
		TNX_ZONE_N("Jobs_RenderWaitForCounter");
		while (counter->Value.load(std::memory_order_acquire) > 0)
		{
			Job job;
			if (s_Queues[static_cast<int>(Queue::Render)].TryPop(job))
			{
				job.EntryPoint(job.Payload, t_ThreadIndex);
				if (job.Counter)
				{
					job.Counter->fetch_sub(1, std::memory_order_release);
					job.Counter->notify_one();
					continue;
				}
			}

			uint32_t snapshot = counter->Value.load(std::memory_order_acquire);

			if (counter->Value.load(std::memory_order_acquire) == 0) break;

			if (s_Queues[static_cast<int>(Queue::Render)].TryPop(job))
			{
				job.EntryPoint(job.Payload, t_ThreadIndex);
				if (job.Counter)
				{
					job.Counter->fetch_sub(1, std::memory_order_release);
					job.Counter->notify_one();
					continue;
				}
			}

			counter->Value.wait(snapshot, std::memory_order_relaxed);
		}
	}

	void LogicWaitForCounter(JobCounter* counter)
	{
		TNX_ZONE_N("Jobs_LogicWaitForCounter");
		while (counter->Value.load(std::memory_order_acquire) > 0)
		{
			Job job;
			if (s_Queues[static_cast<int>(Queue::Physics)].TryPop(job))
			{
				job.EntryPoint(job.Payload, t_ThreadIndex);
				if (job.Counter)
				{
					job.Counter->fetch_sub(1, std::memory_order_release);
					job.Counter->notify_one();
					continue;
				}
			}

			uint32_t snapshot = counter->Value.load(std::memory_order_acquire);

			if (counter->Value.load(std::memory_order_acquire) == 0) break;

			if (s_Queues[static_cast<int>(Queue::Physics)].TryPop(job))
			{
				job.EntryPoint(job.Payload, t_ThreadIndex);
				if (job.Counter)
				{
					job.Counter->fetch_sub(1, std::memory_order_release);
					job.Counter->notify_one();
					continue;
				}
			}

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
