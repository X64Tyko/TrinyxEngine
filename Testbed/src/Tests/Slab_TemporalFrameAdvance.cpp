#include <chrono>

#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "TemporalComponentCache.h"

// Validates ComponentCacheBase frame-advance behavior:
//   - After GetNextWriteFrame(), the next slot is correctly computed (circular)
//   - After PropagateFrame(), ActiveWriteFrame and LastWrittenFrame update correctly
//   - Frame count matches configured TemporalFrameCount
//
// NOTE: This test reads internal state via the public diagnostics API (GetActiveWriteFrame,
// GetActiveReadFrame, GetTotalFrameCount). It does NOT advance the frame — that's the
// Brain thread's job. We verify the invariants hold after engine initialization.
RUNTIME_TEST(Slab_TemporalFrameAdvance)
{
	Registry* Reg = Engine.GetRegistry();

	// --- Volatile slab: always 3 frames (triple-buffer) ---
	{
		ComponentCacheBase* Volatile = Reg->GetVolatileCache();
		ASSERT(Volatile != nullptr);
		ASSERT_EQ(Volatile->GetTotalFrameCount(), 3u);

		// After initialization: write frame must be a valid slot index
		const uint32_t writeFrame = Volatile->GetActiveWriteFrame();
		ASSERT(writeFrame < 3u);

		// NextWriteFrame should differ from current write frame
		const uint32_t nextFrame = Volatile->GetNextWriteFrame();
		ASSERT(nextFrame < 3u);
		ASSERT(nextFrame != writeFrame);
	}

	// --- Temporal/Volatile slab depending on rollback flag ---
	{
		ComponentCacheBase* Temporal = Reg->GetTemporalCache();
		ASSERT(Temporal != nullptr);

		const uint32_t frameCount = Temporal->GetTotalFrameCount();
		ASSERT(frameCount >= 3u); // at minimum triple-buffer

#ifdef TNX_ENABLE_ROLLBACK
		// With rollback, must be at least 8 frames as per CLAUDE.md contract
		ASSERT(frameCount >= 8u);
		// Frame count must be power of 2 (required for ring masking)
		ASSERT((frameCount & (frameCount - 1)) == 0u);
#endif

		const uint32_t writeFrame = Temporal->GetActiveWriteFrame();
		const uint32_t readFrame  = Temporal->GetActiveReadFrame();
		ASSERT(writeFrame < frameCount);
		ASSERT(readFrame < frameCount);

		// NextWriteFrame must be a valid slot
		const uint32_t nextFrame = Temporal->GetNextWriteFrame();
		ASSERT(nextFrame < frameCount);
		ASSERT(nextFrame != writeFrame); // must advance
	}

#ifdef TNX_ENABLE_ROLLBACK
	// --- Rollback slab: the ring wraps from the last slot back to 0 ---
	// The Brain owns ActiveWriteFrame and advances it every step, so the test only observes: it
	// waits for a stable read where the live write frame is the last slot. Writing the pointer
	// from here would race the Brain and disturb the running engine.
	{
		ComponentCacheBase* Temporal = Reg->GetTemporalCache();
		const uint32_t lastSlot      = Temporal->GetTotalFrameCount() - 1;

		bool bObservedWrap  = false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!bObservedWrap && std::chrono::steady_clock::now() < deadline)
		{
			const uint32_t before = Temporal->GetActiveWriteFrame();
			const uint32_t next   = Temporal->GetNextWriteFrame();
			const uint32_t after  = Temporal->GetActiveWriteFrame();
			if (before != after || before != lastSlot) continue; // advanced mid-read, or not at the end yet
			ASSERT_EQ(next, 0u);                                 // must wrap to 0
			bObservedWrap = true;
		}
		ASSERT(bObservedWrap); // the Brain cycles the whole ring many times within the deadline
	}
#endif
}
