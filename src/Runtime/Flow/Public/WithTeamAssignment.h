#pragma once
#include "ReflectionRegistry.h"

class Soul;

// ---------------------------------------------------------------------------
// WithTeamAssignment — mixin for GameModes that assign players to teams.
// The engine detects this interface via dynamic_cast on the active GameMode
// when a Soul joins.
//
// Required overrides:
//   AssignTeam — server must specify which team a joining Soul belongs to
//
// Usage:
//   class CTFMode : public GameMode, public WithTeamAssignment
//   {
//       uint8_t GetTeamCount() const override { return 2; }
//       uint8_t AssignTeam(const Soul& soul) override;
//   };
//
// Engine band ID: 17 (fixed compile-time constant).
// ---------------------------------------------------------------------------
// Not marked final — GameMode subclasses must inherit this directly.
// Treat as effectively sealed: do not create intermediate mixin subclasses.
class WithTeamAssignment
{
public:
static constexpr uint8_t MixinID = 17;

virtual ~WithTeamAssignment() = default;

// Server: number of teams this mode runs.
virtual uint8_t GetTeamCount() const { return 2; }

// Server: assign a team index (0-based) to this Soul on join.
// Pure virtual — the engine has no default assignment policy.
virtual uint8_t AssignTeam(const Soul& soul) = 0;

// Both: called when a Soul's team changes.
virtual void OnTeamChanged(Soul& soul, uint8_t oldTeam, uint8_t newTeam)
{
(void)soul; (void)oldTeam; (void)newTeam;
}

protected:
WithTeamAssignment()
{
static bool registered = []
{
ReflectionRegistry::Get().RegisterMixin("WithTeamAssignment", MixinID, /*isUserDefined=*/false);
return true;
}();
(void)registered;
}
};
