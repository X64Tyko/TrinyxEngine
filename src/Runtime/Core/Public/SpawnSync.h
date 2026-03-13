#pragma once
#include <atomic>
#include <functional>
#include <immintrin.h>
#include <mutex>
#include <thread>

#include "Profiler.h"

class Registry;

// ---------------------------------------------------------------------------
// SpawnSync — Atomic sync-point for safe entity spawning from any thread.
//
// HOW IT WORKS:
//   The calling thread and the Logic thread perform a two-phase handshake:
//
//   1. Caller calls Spawn(action). If caller IS the Logic thread, action
//      executes immediately — no sync overhead.
//
//   2. Otherwise, Caller sets State = Waiting, then spins.
//
//   3. Logic thread, at the top of each frame (before physics), calls
//      SyncPoint(). If State == Waiting, Logic sets State = Granted
//      and freezes — it will not touch entity data until the caller
//      finishes.
//
//   4. Caller sees Granted, executes its spawn work (Create, Hydrate,
//      write fields). The write targets the current write frame because
//      Logic is frozen at a known frame boundary.
//
//   5. Caller sets State = Idle. Logic resumes its frame loop.
//
// USAGE:
//   // Single spawn — blocks until work completes on the correct frame:
//   TrinyxEngine::Get().Spawn([](Registry* reg) {
//       auto ids = reg->Create<CubeEntity<>>(100);
//       WriteCubeSetups(reg, setups, ids);
//   });
//
//   // Batch spawn — one sync point, multiple entity types:
//   TrinyxEngine::Get().Spawn([](Registry* reg) {
//       WriteCubeSetups(reg, cubeSetups, cubeIds);
//       WriteProjectileSetups(reg, projSetups, projIds);
//   });
//
// CONSTRAINTS:
//   - Only ONE thread may be inside Spawn() at a time (enforced by mutex).
//   - Do NOT call Spawn() from inside a Spawn() callback (deadlock).
//   - Before Run() (e.g. in PostInitialize), use Registry::Create directly —
//     no threads are running, no sync is needed.
//   - Spawn() blocks for at most one logic frame (~2ms at 512Hz).
//
// ---------------------------------------------------------------------------

class SpawnSync
{
public:
	/// Spawn entities from any thread. Blocks until the work completes
	/// at a safe point in the Logic thread's frame. If called from the
	/// Logic thread itself, executes immediately.
	void Spawn(std::function<void(Registry*)> action, Registry* reg)
	{
		// No Logic thread yet (pre-Run), or we ARE the Logic thread
		if (LogicId == std::thread::id{} || IsLogicThread())
		{
			action(reg);
			return;
		}

		// Only one spawner at a time through the handshake
		std::lock_guard<std::mutex> lock(SpawnMutex);

		// Signal: "I want to spawn"
		State.store(Waiting, std::memory_order_release);

		// Block until Logic grants permission
		{
			TNX_ZONE_MEDIUM_NC("SpawnSync::Waiting", TNX_COLOR_MEMORY)
			while (State.load(std::memory_order_acquire) != Granted) { _mm_pause(); }
		}

		{
			TNX_ZONE_MEDIUM_NC("SpawnSync::Spawning", TNX_COLOR_MEMORY)
			// Logic is frozen at frame boundary — safe to write
			action(reg);

			// Signal: "I'm done" — Logic resumes
			State.store(Idle, std::memory_order_release);
		}
	}

	/// Called by Brain at the top of each frame (before physics).
	/// If a caller is waiting, grants permission and freezes until
	/// the caller finishes. Otherwise returns immediately.
	void SyncPoint()
	{
		if (State.load(std::memory_order_acquire) != Waiting) return;

		// Grant permission — freeze here while caller writes
		State.store(Granted, std::memory_order_release);

		// Wait for caller to finish
		{
			TNX_ZONE_MEDIUM_NC("SpawnSync::Logic Paused", TNX_COLOR_LOGIC)
			while (State.load(std::memory_order_acquire) != Idle)
			{
			}
		}
	}

	/// Called once by LogicThread::ThreadMain to stamp the logic thread ID.
	void SetLogicThreadId(std::thread::id id) { LogicId = id; }

	bool IsLogicThread() const { return std::this_thread::get_id() == LogicId; }

private:
	static constexpr int Idle    = 0;
	static constexpr int Waiting = 1;
	static constexpr int Granted = 2;

	std::atomic<int> State{Idle};
	std::mutex SpawnMutex; // serializes concurrent Spawn() callers
	std::thread::id LogicId;
};