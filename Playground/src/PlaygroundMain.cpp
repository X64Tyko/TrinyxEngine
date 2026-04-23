#include "TrinyxEngine.h"
#include "GameManager.h"
#include "FlowManagerBase.h"
#include "Logger.h"

#include "GameplayState.h" // Pulls in TNX_REGISTER_STATE(GameplayState) via static init

// ---------------------------------------------------------------------------
// Playground — network playtest application.
// Scene loading is handled by the flow system: EngineConfig::DefaultState
// names a FlowState, and the state's OnEnter loads the level.
// ---------------------------------------------------------------------------
class PlaygroundGame : public GameManager<PlaygroundGame>
{
public:
	const char* GetWindowTitle() const { return "Trinyx Playground"; }

	bool PostInitialize(TrinyxEngine& engine)
	{
		(void)engine;
		LOG_INFO("Playground initialized");
		return true;
	}
};

TNX_IMPLEMENT_GAME(PlaygroundGame)