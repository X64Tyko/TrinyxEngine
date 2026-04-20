#include "TrinyxEngine.h"
#include "GameManager.h"
#include "FlowManager.h"
#include "Logger.h"

#include "DeterminismState.h" // pulls in TNX_REGISTER_STATE(DeterminismState)

// ---------------------------------------------------------------------------
// DeterminismTest — standalone application for rollback determinism testing.
//
// Spawns a player, waits for physics to settle, injects a short burst of
// forward input, then dumps the full temporal ring to the log so you can
// inspect position history for consistency and correctness across rollbacks.
//
// Build with TNX_ENABLE_ROLLBACK=ON, TNX_ENABLE_NETWORK=OFF.
// ---------------------------------------------------------------------------
class DeterminismTestGame : public GameManager<DeterminismTestGame>
{
public:
	const char* GetWindowTitle() const { return "Trinyx Determinism Test"; }

	bool PostInitialize(TrinyxEngine& engine)
	{
		(void)engine;
		LOG_INFO("DeterminismTest initialized — standalone rollback harness");
		return true;
	}
};

TNX_IMPLEMENT_GAME(DeterminismTestGame)
