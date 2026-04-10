#pragma once

#include <cstdint>
#include <thread>

enum class CoreAffinity : uint8_t
{
	OS,
	Input,
	Physics,
	Render,
	Network,
	Worker
};

/**
 * TrinyxThreading — Core-aware thread pinning.
 *
 * Call Initialize() once at startup to scan the CPU topology.
 * Then use PinThread() to assign threads to cores in priority order:
 *   physical cores first (non-SMT), then SMT siblings.
 * Core 0 is skipped (reserved for OS/interrupts).
 *
 * The four coordinator threads (Sentinel, Brain, Encoder, NetThread) each
 * get a dedicated physical core. Remaining cores form the worker pool.
 * On CPUs with fewer than 5 cores, Network shares with Sentinel.
 */
namespace TrinyxThreading
{
	/// Scan CPU topology and build the core assignment list.
	/// Must be called once before any PinThread() calls.
	void Initialize();

	/// Pin the given thread to the next best available core.
	/// Assigns physical cores before SMT siblings. Thread-safe.
	void PinThread(std::thread & t);

	/// Pin the calling thread to a specific core (e.g. for the main/Sentinel thread).
	void PinCurrentThread(uint32_t coreId);

	/// Number of physical (processor) cores detected.
	uint32_t GetPhysicalCoreCount();

	/// Number of logical cores detected (physical + SMT).
	uint32_t GetLogicalCoreCount();

	/// How many cores are available for the worker pool
	/// (logical cores minus the 3 reserved coordinator cores).
	uint32_t GetWorkerThreadCapacity();

	/** Return the ideal core for the requested affinity based on capacity and SMT.
	 *
	 */
	uint32_t GetIdealCore(CoreAffinity affinity);

	/// Whether the CPU has SMT (hyperthreading) enabled.
	bool HasSMT();
}