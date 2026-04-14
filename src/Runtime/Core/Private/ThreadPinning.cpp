#include "ThreadPinning.h"
#include "Logger.h"

#include <algorithm>
#include <atomic>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fstream>
#include <pthread.h>
#include <sched.h>
#include <string>
#endif

namespace TrinyxThreading
{
	// ---- Internal state --------------------------------------------------
	struct CoreEntry
	{
		uint32_t LogicalId;
		uint32_t PhysicalId;
		bool bIsSMT;    // true if this logical core is an SMT sibling
		bool bIsPinned; // true if this core has already been pinned
	};

	static std::vector<CoreEntry> s_CoreList; // priority-ordered (physical first, then SMT)
	static std::atomic<uint32_t> s_NextCore{0};
	static uint32_t s_PhysicalCores = 0;
	static uint32_t s_LogicalCores  = 0;
	static bool s_HasSMT            = false;
	static bool s_Initialized       = false;

	static constexpr uint32_t ReservedCores = 4; // Sentinel, Brain, Encoder, NetThread

	// ---- Platform-specific topology scanning -----------------------------

#ifdef _WIN32

	static void ScanTopology()
	{
		DWORD len = 0;
		GetLogicalProcessorInformation(nullptr, &len);

		std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buffer(
			len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
		GetLogicalProcessorInformation(buffer.data(), &len);

		uint32_t physicalCount = 0;
		uint32_t logicalCount  = 0;

		// First pass: count physical cores and collect core masks
		struct PhysCore
		{
			ULONG_PTR mask;
		};
		std::vector<PhysCore> physCores;

		for (const auto& info : buffer)
		{
			if (info.Relationship == RelationProcessorCore)
			{
				physicalCount++;
				physCores.push_back({info.ProcessorMask});

				// Count logical cores in this physical core
				ULONG_PTR m   = info.ProcessorMask;
				uint32_t bits = 0;
				while (m)
				{
					bits += (m & 1);
					m    >>= 1;
				}
				logicalCount += bits;
				if (bits > 1) s_HasSMT = true;
			}
		}

		s_PhysicalCores = physicalCount;
		s_LogicalCores  = logicalCount;

		// Build core list: physical cores first (lowest logical ID per physical),
		// then SMT siblings, skipping core 0
		std::vector<uint32_t> primaryCores;
		std::vector<uint32_t> smtCores;

		for (const auto& pc : physCores)
		{
			bool firstBit = true;
			for (uint32_t bit = 0; bit < 64; ++bit)
			{
				if (!(pc.mask & (1ULL << bit))) continue;
				if (bit == 0)
				{
					firstBit = false;
					continue;
				} // skip core 0

				uint32_t physId = static_cast<uint32_t>(&pc - physCores.data());
				if (firstBit)
				{
					primaryCores.push_back(bit);
					s_CoreList.push_back({bit, physId, false});
					firstBit = false;
				}
				else
				{
					smtCores.push_back(bit);
					s_CoreList.push_back({bit, physId, true});
				}
			}
		}

		// Re-sort: all primary cores first, then all SMT cores
		std::sort(s_CoreList.begin(), s_CoreList.end(), [](const CoreEntry& a, const CoreEntry& b)
		{
			if (a.bIsSMT != b.bIsSMT) return !a.bIsSMT; // physical first
			return a.LogicalId < b.LogicalId;
		});
	}

#else // Linux/POSIX

	static uint32_t ReadSysfsInt(const char* path, uint32_t fallback)
	{
		std::ifstream f(path);
		if (!f.is_open()) return fallback;
		uint32_t val = fallback;
		f >> val;
		return val;
	}

	static void ScanTopology()
	{
		// Get the set of cores available to this process
		cpu_set_t available;
		CPU_ZERO(&available);
		sched_getaffinity(0, sizeof(cpu_set_t), &available);

		// Map logical core -> physical core ID via sysfs
		struct LogicalCore
		{
			uint32_t logicalId;
			uint32_t physicalId;
		};
		std::vector<LogicalCore> cores;

		uint32_t maxLogical = std::thread::hardware_concurrency();
		if (maxLogical == 0) maxLogical = CPU_SETSIZE;

		for (uint32_t i = 0; i < maxLogical; ++i)
		{
			if (!CPU_ISSET(i, &available)) continue;

			char path[128];
			snprintf(path, sizeof(path),
					 "/sys/devices/system/cpu/cpu%u/topology/core_id", i);
			uint32_t physId = ReadSysfsInt(path, i); // fallback: treat logical == physical
			cores.push_back({i, physId});
		}

		s_LogicalCores = static_cast<uint32_t>(cores.size());

		// Count unique physical core IDs
		std::vector<uint32_t> uniquePhys;
		for (const auto& c : cores)
		{
			if (std::find(uniquePhys.begin(), uniquePhys.end(), c.physicalId) == uniquePhys.end()) uniquePhys.push_back(c.physicalId);
		}
		s_PhysicalCores = static_cast<uint32_t>(uniquePhys.size());
		s_HasSMT        = (s_LogicalCores > s_PhysicalCores);

		// Mark first logical core per physical as primary, rest as SMT
		// Track which physical cores we've already seen a "primary" for
		std::vector<uint32_t> seenPhysical;

		for (const auto& c : cores)
		{
			if (c.logicalId == 0) continue; // skip core 0

			bool isPrimary = (std::find(seenPhysical.begin(), seenPhysical.end(), c.physicalId)
				== seenPhysical.end());
			if (isPrimary) seenPhysical.push_back(c.physicalId);

			s_CoreList.push_back({c.logicalId, c.physicalId, !isPrimary});
		}

		// Sort: physical cores first, then SMT, each group ascending by logical ID
		std::sort(s_CoreList.begin(), s_CoreList.end(), [](const CoreEntry& a, const CoreEntry& b)
		{
			if (a.bIsSMT != b.bIsSMT) return !a.bIsSMT; // physical first
			return a.LogicalId < b.LogicalId;
		});
	}

#endif // platform

	// ---- Public API ------------------------------------------------------

	void Initialize()
	{
		if (s_Initialized) return;

		s_CoreList.clear();
		s_NextCore.store(0, std::memory_order_relaxed);

		ScanTopology();
		s_Initialized = true;

		LOG_ENG_INFO_F("[ThreadPinning] Detected %u physical cores, %u logical cores (SMT: %s)",
					   s_PhysicalCores, s_LogicalCores, s_HasSMT ? "yes" : "no");
		LOG_ENG_INFO_F("[ThreadPinning] %u cores available for pinning (skipping core 0)",
					   static_cast<uint32_t>(s_CoreList.size()));
		LOG_ENG_INFO_F("[ThreadPinning] Worker pool capacity: %u threads", GetWorkerThreadCapacity());
	}

	void PinThread(std::thread& t)
	{
		if (s_CoreList.empty()) return;

		CoreEntry& core = s_CoreList[0];
		do
		{
			uint32_t idx          = s_NextCore.fetch_add(1, std::memory_order_relaxed);
			uint32_t wrapped      = idx % static_cast<uint32_t>(s_CoreList.size());
			core = s_CoreList[wrapped];
		} while (core.bIsPinned);

#ifdef _WIN32
		DWORD_PTR mask = (1ULL << core.LogicalId);
		SetThreadAffinityMask(reinterpret_cast<HANDLE>(t.native_handle()), mask);
#else
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(core.LogicalId, &cpuset);
		pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
#endif

		core.bIsPinned = true;
		LOG_ENG_INFO_F("[ThreadPinning] Pinned thread to logical core %u (physical %u, %s)",
					   core.LogicalId, core.PhysicalId, core.bIsSMT ? "SMT" : "primary");
	}

	uint32_t GetIdealCore(CoreAffinity affinity)
	{
		if (s_CoreList.empty()) return -1;

		switch (affinity)
		{
			case CoreAffinity::OS: return 0;
			case CoreAffinity::Input: return s_CoreList[0].LogicalId;
			case CoreAffinity::Physics: return s_CoreList[1].LogicalId;
			case CoreAffinity::Render: return s_CoreList[2].LogicalId;
			case CoreAffinity::Network:
				// On CPUs with 4+ available cores, Network gets its own core.
				// Otherwise shares with Sentinel (core index 0 = Input).
				return s_CoreList.size() > 3 ? s_CoreList[3].LogicalId : s_CoreList[0].LogicalId;
			case CoreAffinity::Worker: return -1; // > core count, will auto decide
		}
		
		return -1;
	}

	void PinCurrentThread(uint32_t coreId)
	{
#ifdef _WIN32
		DWORD_PTR mask = (1ULL << coreId);
		SetThreadAffinityMask(GetCurrentThread(), mask);
#else
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(coreId, &cpuset);
		pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
		
		for (auto& core : s_CoreList)
		{
			if (core.LogicalId == coreId)
			{
				core.bIsPinned = true;
				break;
			}
		}

		LOG_ENG_INFO_F("[ThreadPinning] Pinned current thread to core %u", coreId);
	}

	uint32_t GetPhysicalCoreCount() { return s_PhysicalCores; }
	uint32_t GetLogicalCoreCount() { return s_LogicalCores; }

	uint32_t GetWorkerThreadCapacity()
	{
		uint32_t available = HasSMT() ? s_PhysicalCores : static_cast<uint32_t>(s_CoreList.size());
		return (available > ReservedCores) ? (available - ReservedCores) : 0;
	}

	bool HasSMT() { return s_HasSMT; }
}
