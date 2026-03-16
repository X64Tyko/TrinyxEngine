#include "TrinyxEngine.h"
#include "GameManager.h"
#include "EntityBuilder.h"
#include "Logger.h"
#include "Registry.h"

#include <string>

// ---------------------------------------------------------------------------
// DemoGame — loads a scene file from content/ via EntityBuilder.
// ---------------------------------------------------------------------------
class DemoGame : public GameManager<DemoGame>
{
public:
	const char* GetWindowTitle() const { return "Trinyx Demo"; }

	bool PostInitialize(TrinyxEngine& engine)
	{
		(void)engine;
		LOG_INFO("DemoGame initialized");
		return true;
	}

	void PostStart(TrinyxEngine& engine)
	{
		std::string scenePath = std::string(TNX_PROJECT_DIR) + "/content/TestScene.tnxscene";
		LOG_INFO_F("Loading scene: %s", scenePath.c_str());

		engine.Spawn([&scenePath](Registry* reg)
		{
			EntityBuilder::SpawnFromFile(reg, scenePath.c_str());
		});
	}
};

TNX_IMPLEMENT_GAME(DemoGame)