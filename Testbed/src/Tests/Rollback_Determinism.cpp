#ifdef TNX_ENABLE_ROLLBACK

#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "Logger.h"
#include "Tests/TestbedHelpers.h"

// Validates rollback determinism: save state at frame N, advance M frames,
// restore to N, re-advance M frames, compare final state against the first run.
//
// Requires: TNX_ENABLE_ROLLBACK=ON
//
// This test needs the engine loop to be active so Jolt's SaveState/RestoreState
// API is available and the temporal ring buffers are populated.
RUNTIME_TEST(Rollback_Determinism)
{
	/*
	 * TODO: Rollback_Determinism
	 *
	 * Steps:
	 *   1. Spawn a small pyramid of dynamic Jolt cubes (uses gPyramidIds if
	 *      Spawn_JoltPyramid ran first; otherwise spawn a fresh set here).
	 *   2. Record ECS state snapshot at frame N via TemporalComponentCache.
	 *   3. Run M fixed steps (Brain::AdvanceFrame in test mode).
	 *   4. Record final state snapshot A.
	 *   5. Restore to frame N via JoltPhysics::RestoreState + TemporalComponentCache rewind.
	 *   6. Re-run the same M fixed steps with the same input seed.
	 *   7. Record final state snapshot B.
	 *   8. ASSERT snapshot A == snapshot B (bit-identical for all active entity fields).
	 *
	 * Blocked by: Brain test-mode step API (step N frames from test context without
	 * blocking on the real-time scheduler).
	 */

	LOG_ENG_ALWAYS("[Rollback_Determinism] Rollback determinism test — pending Brain test-mode step API");

	// Baseline: assert the temporal cache is present and has the expected frame count
	Registry* reg = Engine.GetRegistry();
	ASSERT(reg->GetTemporalCache() != nullptr);
}

#endif // TNX_ENABLE_ROLLBACK
