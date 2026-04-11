#pragma once
#include "ReflectionRegistry.h"

class Soul;

// ---------------------------------------------------------------------------
// WithLobby — mixin for GameModes that gate match start behind a ready-check
// or lobby phase. The engine detects this interface via dynamic_cast on the
// active GameMode when processing player-ready signals.
//
// Required overrides:
//   OnLobbyReady — called when all readiness conditions are met; the GameMode
//                  must decide what happens (countdown, travel, etc.)
//
// Usage:
//   class RoyaleMode : public GameMode, public WithLobby
//   {
//       uint8_t GetMinPlayers() const override { return 10; }
//       void    OnLobbyReady() override { StartCountdown(); }
//   };
//
// Engine band ID: 18 (fixed compile-time constant).
// ---------------------------------------------------------------------------
// Not marked final — GameMode subclasses must inherit this directly.
// Treat as effectively sealed: do not create intermediate mixin subclasses.
class WithLobby
{
public:
static constexpr uint8_t MixinID = 18;

virtual ~WithLobby() = default;

// Server: minimum number of ready souls before OnLobbyReady fires.
virtual uint8_t GetMinPlayers() const { return 2; }

// Server: all readiness conditions met — start countdown, send manifest, etc.
// Pure virtual — the engine has no default response to lobby completion.
virtual void OnLobbyReady() = 0;

// Server: a Soul signalled ready. Default increments ReadyCount and calls
// CheckLobbyReady(). Override to add validation logic.
virtual void OnPlayerReady(Soul& soul) { (void)soul; ++ReadyCount; CheckLobbyReady(); }

// Server: a Soul withdrew ready.
virtual void OnPlayerUnready(Soul& soul) { (void)soul; if (ReadyCount > 0) --ReadyCount; }

protected:
// Fires OnLobbyReady if ReadyCount >= GetMinPlayers(). Override to replace
// the evaluation logic (e.g. check party composition, not just raw count).
virtual void CheckLobbyReady()
{
if (ReadyCount >= GetMinPlayers())
OnLobbyReady();
}

uint8_t ReadyCount = 0;

WithLobby()
{
static bool registered = []
{
ReflectionRegistry::Get().RegisterMixin("WithLobby", MixinID, /*isUserDefined=*/false);
return true;
}();
(void)registered;
}
};
