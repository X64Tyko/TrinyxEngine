#pragma once

#include "Construct.h"
#include "ConstructView.h"
#include "EPlayer.h"

// ---------------------------------------------------------------------------
// Body<TEntity> — World-lifetime Construct wrapping an ECS entity for a
// player or bot body.
//
// Body is the engine anchor for a participant's in-world presence. It wraps a
// ConstructView of any entity type so different games can use EHero, EVehicle,
// etc. without changing the engine spawn flow. The default is EPlayer.
//
// Ownership
//   Server: created by GameMode::OnPlayerJoined (or ValidateSpawnRequest).
//           OwnerNetHandle assigned by the replication system at spawn.
//   Client: created predictively after SpawnRequest is sent. OwnerNetHandle
//           is set to zero until SpawnConfirm arrives and wires the handle.
//
// Subclassing
//   Game code subclasses Body to add movement logic, health, abilities, etc.:
//
//     class PlayerBody : public Body<EPlayer>
//     {
//         void OnSpawned();              // auto-called after views hydrate (Construct hook)
//         void PrePhysics(SimFloat dt);  // auto-registered via Construct<T>
//     };
// ---------------------------------------------------------------------------
template <typename TEntity = EPlayer>
class Body : public Construct<Body<TEntity>>
{
public:
	// World-lifetime — destroyed when the World resets or is torn down.
	static constexpr ConstructLifetime Lifetime = ConstructLifetime::World;

	uint32_t OwnerNetHandle = 0; // Confirmed by server at SpawnConfirm (0 = predicted/unconfirmed)
	uint8_t  OwnerID        = 0; // NetOwnerID of the controlling Soul

	ConstructView<TEntity> View;

	void InitializeViews()
	{
		View.Initialize(this);
	}

protected:
	Body() = default;
};
