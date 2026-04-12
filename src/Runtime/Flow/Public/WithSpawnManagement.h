#pragma once
#include "ReflectionRegistry.h"
#include "RegistryTypes.h"

class Soul;
struct PlayerBeginRequestPayload;

// ---------------------------------------------------------------------------
// WithSpawnManagement — mixin for GameModes that own the player body spawn
// pipeline. The engine detects this interface via dynamic_cast on the active
// GameMode when a PlayerBeginRequest message arrives.
//
// Required overrides:
//   GetCharacterPrefab — server must specify what to spawn per Soul
//
// Usage:
//   class ArenaMode : public GameMode, public WithSpawnManagement
//   {
//       int64_t GetCharacterPrefab(const Soul& soul) const override;
//   };
//
// Engine band ID: 16 (fixed compile-time constant).
// ---------------------------------------------------------------------------
// Not marked final — GameMode subclasses must inherit this directly.
// Treat as effectively sealed: do not create intermediate mixin subclasses.
class WithSpawnManagement
{
public:
static constexpr uint8_t MixinID = 16;

virtual ~WithSpawnManagement() = default;

// Server: return the prefab UUID to spawn for this Soul.
// Pure virtual — the engine has no default spawn policy.
virtual int64_t GetCharacterPrefab(const Soul& soul) const = 0;

// Server: return true to allow the spawn, false to issue PlayerBeginReject.
// Override to enforce cooldowns, team limits, phase gates, etc.
virtual bool ValidateSpawn(const Soul& soul, const PlayerBeginRequestPayload& req)
{
(void)soul; (void)req; return true;
}

// Both: called when a body is confirmed live (Soul::ClaimBody already called).
virtual void OnSpawnConfirmed(Soul& soul, ConstructRef bodyRef)
{
(void)soul; (void)bodyRef;
}

// Both: called when a body is lost (Soul::ReleaseBody already called).
virtual void OnSpawnLost(Soul& soul) { (void)soul; }

protected:
WithSpawnManagement()
{
static bool registered = []
{
ReflectionRegistry::Get().RegisterMixin("WithSpawnManagement", MixinID, /*isUserDefined=*/false);
return true;
}();
(void)registered;
}
};
