#include "TrinyxEngine.h"
#include "GameManager.h"
#include "Logger.h"

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
};

TNX_IMPLEMENT_GAME(DemoGame)