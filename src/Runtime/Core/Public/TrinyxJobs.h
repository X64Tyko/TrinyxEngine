#pragma once

#include <atomic>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "TrinyxRingBuffer.h"

struct EngineConfig;

/**
 * TrinyxJobs — Lock-free job system for the Trinyx Trinity threading model.
 *
 * Three queues with affinity:
 *   Physics  — Brain (LogicThread) produces, workers + Brain consume
 *   Render   — Encoder (VulkRender) produces, workers + Encoder consume
 *   General  — any thread produces/consumes
 *
 * Usage:
 *   TrinyxJobs::JobCounter counter;
 *   for (auto* chunk : chunks)
 *       TrinyxJobs::Dispatch([=](uint32_t) { ProcessChunk(chunk, dt); },
 *                            &counter, TrinyxJobs::Queue::Physics);
 *   TrinyxJobs::WaitForCounter(&counter);  // caller steals work while waiting
 *
 * Jolt integration: write a thin JPH::JobSystem adapter that maps
 * CreateJob/QueueJob/Barrier onto Dispatch/WaitForCounter.
 */
namespace TrinyxJobs
{
	// ---- Queue affinity --------------------------------------------------

	enum class Queue : uint8_t
	{
		Physics = 1 << 0, // PhysicsThread work (Physics, Collision)
		Logic   = 1 << 1, // LogicThread work (PrePhysics, PostPhysics)
		Render  = 1 << 2, // RenderThread work (GPU upload, compute dispatch)
		General = 1 << 3, // Anything else

		COUNT = 4,
		All   = Physics | Logic | Render | General
	};

	constexpr Queue operator|(Queue a, Queue b)
	{
		return static_cast<Queue>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
	}

	constexpr Queue operator&(Queue a, Queue b)
	{
		return static_cast<Queue>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
	}

	// ---- Job lambda validation -------------------------------------------

	/// A valid job lambda must:
	///  - Accept (uint32_t threadIndex), return void
	///  - Fit in 48 bytes (captures + vtable-free)
	///  - Be trivially copyable (memcpy-safe, no destructor)
	template <typename T>
	concept ValidJobLambda = requires(T t, uint32_t threadIdx)
	{
		{ t(threadIdx) } -> std::same_as<void>;
	} && sizeof(T) <= 48 && std::is_trivially_copyable_v<T>;

	// ---- Job struct (one cache line) -------------------------------------

	struct alignas(64) Job
	{
		void (*EntryPoint)(void* payload, uint32_t threadIdx) = nullptr;
		std::atomic<uint32_t>* Counter                        = nullptr;
		uint8_t Payload[48]{}; // lambda capture storage
	};

	static_assert(sizeof(Job) == 64, "Job must fit in one cache line");

	// ---- Completion counter ----------------------------------------------

	/// Stack-allocatable completion counter.
	/// Dispatch increments, job completion decrements. WaitForCounter blocks until 0.
	struct JobCounter
	{
		std::atomic<uint32_t> Value{0};
	};

	// ---- Lifecycle -------------------------------------------------------

	/// Initialize queues and spawn worker threads (count from ThreadPinning).
	bool Initialize(const EngineConfig* config);

	/// Signal workers to stop and join all threads. Safe to call multiple times.
	void Shutdown();

	// ---- Dispatch --------------------------------------------------------

	/// Submit a pre-built Job to the specified queue.
	/// Called by Dispatch<> template — not typically used directly.
	void SubmitJob(const Job& job, Queue queue);

	/// Dispatch a lambda as a job. Increments counter before submission.
	/// All Dispatches for a given counter must complete before WaitForCounter.
	template <ValidJobLambda LAMBDA>
	void Dispatch(LAMBDA lambda, JobCounter* counter, Queue queue = Queue::General)
	{
		// Build the trampoline: a plain function pointer that reinterprets
		// the payload back to the lambda type and invokes it.
		struct Trampoline
		{
			static void Invoke(void* payload, uint32_t threadIdx)
			{
				auto* fn = reinterpret_cast<LAMBDA*>(payload);
				(*fn)(threadIdx);
			}
		};

		Job job;
		job.EntryPoint = &Trampoline::Invoke;
		job.Counter    = &counter->Value;
		std::memcpy(job.Payload, &lambda, sizeof(LAMBDA));

		// Increment BEFORE submission so counter never transiently hits 0
		counter->Value.fetch_add(1, std::memory_order_relaxed);
		SubmitJob(job, queue);
	}

	// ---- Synchronization -------------------------------------------------

	/// Spin until the counter reaches 0, stealing work from all queues while waiting.
	/// The calling thread becomes a worker during the wait.
	void WaitForCounter(JobCounter* counter, Queue affinity = Queue::All);

	// ---- Queries ---------------------------------------------------------

	/// Number of active worker threads (excludes coordinators).
	uint32_t GetWorkerCount();

	/// Thread-local index of the current worker (0 = coordinator/non-worker).
	uint32_t GetCurrentThreadIndex();
}
