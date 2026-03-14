#pragma once

class TrinyxEngine;

/**
 * GameManager<Derived> — CRTP base class for user game projects.
 *
 * Subclass this in your project and use TNX_IMPLEMENT_GAME(YourClass)
 * in one .cpp file to wire up main() automatically. The engine will
 * call PostInitialize() after all core systems are ready but before
 * the main loop starts.
 *
 * Override any method by name-hiding (no virtual, no vtable):
 *
 *   class MyGame : public GameManager<MyGame> {
 *   public:
 *       const char* GetWindowTitle() const { return "My Game"; }
 *       bool PostInitialize(TrinyxEngine& engine) { ... return true; }
 *   };
 *   TNX_IMPLEMENT_GAME(MyGame)
 *
 * Without the macro, users can drive initialization manually:
 *
 *   MyGame game;
 *   auto& engine = TrinyxEngine::Get();
 *   engine.Initialize(game.GetWindowTitle(), game.GetWindowWidth(),
 *                     game.GetWindowHeight(), projectDir);
 *   game.PostInitialize(engine);
 *   engine.Run();
 */
template <typename Derived>
class GameManager
{
public:
	/// Called after engine initialization completes (all threads, renderer, registry ready).
	/// Use this to spawn initial entities, load levels, etc.
	/// Return false to abort before entering the main loop.
	bool PostInitialize(TrinyxEngine& engine)
	{
		(void)engine;
		return true;
	}

	/// Called from Run() after Logic, Render, and the job system are fully started.
	/// Use this for runtime tests or setup that requires the engine loop to be active.
	void PostStart(TrinyxEngine& engine)
	{
		(void)engine;
	}

	const char* GetWindowTitle() const { return "Trinyx Game"; }
	int GetWindowWidth() const { return 1920; }
	int GetWindowHeight() const { return 1080; }

protected:
	Derived& Self() { return static_cast<Derived&>(*this); }
	const Derived& Self() const { return static_cast<const Derived&>(*this); }
};

/**
 * TNX_IMPLEMENT_GAME(GameClass)
 *
 * Place in exactly one .cpp file in your game project. Provides main()
 * and wires up engine initialization -> PostInitialize -> Run.
 *
 * Requires TNX_PROJECT_DIR to be defined by CMake (set automatically
 * when using the standard project CMakeLists pattern).
 */
#ifndef TNX_PROJECT_DIR
#define TNX_PROJECT_DIR ""
#endif

#define TNX_IMPLEMENT_GAME(GameClass)                                                    \
	int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])                   \
	{                                                                                    \
		TrinyxEngine& engine = TrinyxEngine::Get();                                      \
		GameClass game;                                                                  \
		if (engine.Initialize(game.GetWindowTitle(),                                     \
		                      game.GetWindowWidth(),                                     \
		                      game.GetWindowHeight(),                                    \
		                      TNX_PROJECT_DIR))                                           \
		{                                                                                \
			if (game.PostInitialize(engine))                                             \
				engine.Run(game);                                                            \
		}                                                                                \
		return 0;                                                                        \
	}
