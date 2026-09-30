#pragma once

#include <atomic>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "TrinyxMPMCRing.h"

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

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
	Logic   = 1 << 0, // LogicThread work (PrePhysics, PostPhysics)
	Render  = 1 << 1, // RenderThread work (GPU upload, compute dispatch)
	Physics = 1 << 2, // PhysicsThread work (Physics, Collision)
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
concept ValidJobLambda = requires(T t, uint32_t threadIdx) {
	{ t(threadIdx) } -> std::same_as<void>;
} && sizeof(T) <= 48 && std::is_trivially_copyable_v<T>;

/// A valid named job lambda — same as ValidJobLambda but must fit in 40 bytes.
/// DispatchNamed packs {name*(8) + lambda(40)} into the 48-byte Job payload.
template <typename T>
concept ValidNamedJobLambda = requires(T t, uint32_t threadIdx) {
	{ t(threadIdx) } -> std::same_as<void>;
} && sizeof(T) <= 40 && std::is_trivially_copyable_v<T>;

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
	std::atomic<uint32_t> Value{ 0 };
};

// ---- Lifecycle -------------------------------------------------------

/// Initialize queues and spawn worker threads (count from ThreadPinning).
bool Initialize(const EngineConfig* config);

/// Signal workers to stop and join all threads. Safe to call multiple times.
void Shutdown();

/// Returns true if the job system has been initialized and workers are running.
/// Check before dispatching from code that may outlive the engine (e.g. net threads).
bool IsRunning();

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

/// Dispatch a named lambda as a job. Identical to Dispatch but emits a profiler zone
/// with the given name when the job executes. Name must be a string literal (static storage).
/// Lambda captures are limited to 40 bytes (vs. 48 for Dispatch) — the name pointer
/// occupies the remaining 8 bytes of the 48-byte payload.
template <ValidNamedJobLambda LAMBDA>
void DispatchNamed(const char* name, LAMBDA lambda, JobCounter* counter, Queue queue = Queue::General)
{
	struct Capture
	{
		const char* name;
		LAMBDA fn;
	};
	static_assert(sizeof(Capture) <= 48, "Named job capture exceeds payload — reduce lambda captures");

	struct Trampoline
	{
		static void Invoke(void* payload, uint32_t threadIdx)
		{
			auto* c = reinterpret_cast<Capture*>(payload);
#ifdef TRACY_ENABLE
			ZoneScoped;
			ZoneName(c->name, std::strlen(c->name));
#endif
			c->fn(threadIdx);
		}
	};

	Capture cap{ name, lambda };
	Job job;
	job.EntryPoint = &Trampoline::Invoke;
	job.Counter    = &counter->Value;
	std::memcpy(job.Payload, &cap, sizeof(Capture));

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

// ---- World queues ----------------------------------------------------
// Per-world ring buffers drained only by the owning LogicThread.
// Use World::Post / World::Spawn / World::SpawnAndWait — not these directly.

using WorldQueueHandle                              = uint32_t;
static constexpr WorldQueueHandle InvalidWorldQueue = ~0u;

WorldQueueHandle CreateWorldQueue(size_t capacity = 256);
void DestroyWorldQueue(WorldQueueHandle handle);
void DrainWorldQueue(WorldQueueHandle handle);

/// Internal — called by Post/Spawn/SpawnAndWait templates.
void SubmitWorldJob(const Job& job, WorldQueueHandle handle);

template <ValidJobLambda LAMBDA>
void Post(LAMBDA lambda, WorldQueueHandle handle)
{
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
	job.Counter    = nullptr;
	std::memcpy(job.Payload, &lambda, sizeof(LAMBDA));
	SubmitWorldJob(job, handle);
}

template <ValidJobLambda LAMBDA>
void Spawn(LAMBDA lambda, JobCounter* counter, WorldQueueHandle handle)
{
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

	counter->Value.fetch_add(1, std::memory_order_relaxed);
	SubmitWorldJob(job, handle);
}

template <ValidJobLambda LAMBDA>
void SpawnAndWait(LAMBDA lambda, WorldQueueHandle handle)
{
	JobCounter counter;
	Spawn(lambda, &counter, handle);
	while (counter.Value.load(std::memory_order_acquire) > 0)
	{
		counter.Value.wait(1, std::memory_order_relaxed);
	}
}
} // namespace TrinyxJobs
