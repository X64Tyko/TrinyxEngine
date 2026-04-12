#include "GameMode.h"
#include "Soul.h"
#include "WithSpawnManagement.h"

// Out-of-line destructor — anchors the vtable and typeinfo so dynamic_cast works
// from translation units that only see the header (e.g. FlowManager.cpp).
GameMode::~GameMode() = default;

PlayerBeginResult GameMode::OnPlayerBeginRequest(Soul& soul, const PlayerBeginRequestPayload& req)
{
	PlayerBeginResult result;

	// Opt-in validation: if this GameMode mixes in WithSpawnManagement, run its gate.
	if (auto* wsm = dynamic_cast<WithSpawnManagement*>(this))
	{
		if (!wsm->ValidateSpawn(soul, req))
			return result; // Accepted = false
	}

	// Default: accept unconditionally, echo client's position hint.
	// Game code overrides OnPlayerBeginRequest to pick a real spawn point and create a Body.
	result.Accepted = true;
	result.PosX     = req.PosX;
	result.PosY     = req.PosY;
	result.PosZ     = req.PosZ;

	soul.ClaimBody(result.Body);

	// Notify the mixin that a body is now confirmed.
	if (auto* wsm = dynamic_cast<WithSpawnManagement*>(this))
		wsm->OnSpawnConfirmed(soul, result.Body);

	return result;
}
