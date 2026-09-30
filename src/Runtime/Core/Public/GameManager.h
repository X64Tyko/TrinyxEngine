#pragma once

class TrinyxEngine;

/** @addtogroup core
 *  @{
 */

/**
 * @brief CRTP base for user game projects; provides default lifecycle hooks and window config.
 *
 * @tparam Derived Concrete game class. Override hooks by name-hiding — no virtual dispatch.
 *
 * Usage: subclass, override what you need, then place @ref TNX_IMPLEMENT_GAME in one .cpp:
 * @code
 *   class MyGame : public GameManager<MyGame> {
 *   public:
 *       const char* GetWindowTitle() const { return "My Game"; }
 *       bool PostInitialize(TrinyxEngine& engine) { ... return true; }
 *   };
 *   TNX_IMPLEMENT_GAME(MyGame)
 * @endcode
 *
 * Without the macro, drive initialization manually via `TrinyxEngine::Initialize` /
 * `PostInitialize` / `Run`.
 */
template <typename Derived>
class GameManager
{
public:
	/// @brief Called after ParseCommandLine but before engine Initialize().
	/// @note Use this to parse game-specific CLI arguments before any engine system starts.
	void PreInitialize(int argc, char* argv[])
	{
		(void)argc;
		(void)argv;
	}

	/// @brief Called once all engine threads, renderer, and registry are ready.
	/// @return false to abort before entering the main loop.
	bool PostInitialize(TrinyxEngine& engine)
	{
		(void)engine;
		return true;
	}

	/// @brief Called from Run() after Logic, Render, and the job system are fully started.
	/// @note Use this for runtime tests or setup that requires live tick dispatch.
	void PostStart(TrinyxEngine& engine)
	{
		(void)engine;
	}

	/// @brief Returns the process exit code after Run() completes.
	/// @note Override to surface post-start failure counts (e.g. runtime test failures).
	/// TNX_IMPLEMENT_GAME returns this value directly.
	int GetExitCode() const { return 0; }

	const char* GetWindowTitle() const { return "Trinyx Game"; } ///< Override to set window title.
	int GetWindowWidth() const { return 1920; }                  ///< Override to set initial window width.
	int GetWindowHeight() const { return 1080; }                 ///< Override to set initial window height.

protected:
	Derived& Self() { return static_cast<Derived&>(*this); }                   ///< CRTP downcast.
	const Derived& Self() const { return static_cast<const Derived&>(*this); } ///< CRTP downcast (const).
};

/**
 * @brief Provides `main()` and wires `ParseCommandLine → Initialize → PostInitialize → Run`.
 *
 * Place in exactly one .cpp in your game project.
 * @note Requires `TNX_PROJECT_DIR` to be defined by CMake (set automatically by the standard
 *       project CMakeLists pattern).
 */
#ifndef TNX_PROJECT_DIR
#define TNX_PROJECT_DIR ""
#endif

#define TNX_IMPLEMENT_GAME(GameClass)                                  \
	int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) \
	{                                                                  \
		TrinyxEngine& engine = TrinyxEngine::Get();                    \
		engine.ParseCommandLine(argc, argv);                           \
		GameClass game;                                                \
		game.PreInitialize(argc, argv);                                \
		int exitCode = 1;                                              \
		if (engine.Initialize(game.GetWindowTitle(),                   \
				game.GetWindowWidth(),                                 \
				game.GetWindowHeight(),                                \
				TNX_PROJECT_DIR))                                      \
		{                                                              \
			if (game.PostInitialize(engine))                           \
			{                                                          \
				engine.Run(game);                                      \
				exitCode = game.GetExitCode();                         \
			}                                                          \
			else                                                       \
			{                                                          \
				exitCode = 1;                                          \
			}                                                          \
		}                                                              \
		return exitCode;                                               \
	}

/** @} */
