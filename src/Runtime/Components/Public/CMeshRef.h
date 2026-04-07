#pragma once
#include "ComponentView.h"
#include "SchemaReflector.h"

// MeshRef component — render mesh and material references.
// Cold: stored in archetype chunks (AoS). Set at spawn, rarely changes.
// Drives GPU state-sorted rendering: MeshID + MaterialID feed the 64-bit sort key.
// Render partition group (Render).
//
// Cold component interface: plain POD fields, no FieldProxy, no SoA allocation.
// Bind() is a no-op; chunk access goes through Archetype::GetComponentArray.
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct CMeshRef : ComponentView<CMeshRef, WIDTH>
{
	UIntProxy<WIDTH> MeshID{};     // Asset system mesh handle (default 0)
	UIntProxy<WIDTH> MaterialID{}; // Asset system material handle (default 0)
	UIntProxy<WIDTH> LODCount{};   // Number of LOD levels available
	UIntProxy<WIDTH> CastShadow{}; // 1 = participates in shadow passes

	// Per-field asset type annotation — editor displays name instead of raw index
	static constexpr auto FieldRefTypes = std::array{
		AssetType::StaticMesh, // MeshID
		AssetType::Material,   // MaterialID
		AssetType::Invalid,    // LODCount
		AssetType::Invalid,    // CastShadow
	};

	TNX_VOLATILE_FIELDS(CMeshRef, Render, MeshID, MaterialID, LODCount, CastShadow)
};

TNX_REGISTER_COMPONENT(CMeshRef)
