#include "TrinyxEngine.h"
#include "GameManager.h"
#include "EntityBuilder.h"
#include "Logger.h"
#include "Registry.h"

#include <string>

// ---------------------------------------------------------------------------
// Playground — network playtest application.
// Loads scene from content/ in non-editor builds.
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

	void PostStart(TrinyxEngine& engine)
	{
#if !TNX_ENABLE_EDITOR
		// Non-editor builds load the default scene from config.
		// Editor builds load it via EditorContext which also tracks the path.
		const EngineConfig* cfg = engine.GetConfig();
		if (cfg->InitialGameScene[0] != '\0' && cfg->ProjectDir[0] != '\0')
		{
			std::string scenePath = std::string(cfg->ProjectDir) + "/content/" + cfg->InitialGameScene;
			LOG_INFO_F("Loading scene: %s", scenePath.c_str());
			engine.Spawn([scenePath](Registry* reg)
			{
				EntityBuilder::SpawnFromFile(reg, scenePath.c_str());
			});
		}
#else
		(void)engine;
#endif
	}
};

TNX_IMPLEMENT_GAME(PlaygroundGame)