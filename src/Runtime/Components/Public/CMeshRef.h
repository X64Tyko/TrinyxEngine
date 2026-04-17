#pragma once
#include "ComponentView.h"
#include "SchemaReflector.h"
#include "AssetRegistry.h"

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

	// --- Init-lambda assignment API (Scalar only) ---

	// Checks out MeshID and, if the entry has a DefaultMaterial, a conditional MaterialID checkout.
	void SetMesh(TnxName name) requires (WIDTH == FieldWidth::Scalar)
	{
		const AssetEntry* entry = AssetRegistry::Get().FindByTName(name);
		if (!entry)
		{
			LOG_ENG_WARN_F("CMeshRef::SetMesh - mesh '%s' not found in asset registry", name.GetStr());
			return;
		}

		AssetRegistry::RegisterPendingCheckout(&MeshID.WriteArray[MeshID.index], name);

		if (entry->DefaultMaterial.Value != 0) AssetRegistry::RegisterPendingCheckout(&MaterialID.WriteArray[MaterialID.index], entry->DefaultMaterial, true);
	}

	// Checks out MaterialID only.
	void SetMaterial(TnxName name) requires (WIDTH == FieldWidth::Scalar)
	{
		AssetRegistry::RegisterPendingCheckout(&MaterialID.WriteArray[MaterialID.index], name);
	}

	// Dispatches to SetMesh or SetMaterial based on the catalogued asset type.
	CMeshRef& operator=(TnxName name) requires (WIDTH == FieldWidth::Scalar)
	{
		const AssetEntry* entry = AssetRegistry::Get().FindByTName(name);
		if (!entry)
		{
			LOG_ENG_WARN_F("CMeshRef::operator= - asset '%s' not found in asset registry", name.GetStr());
			return *this;
		}

		if (entry->Type == AssetType::StaticMesh || entry->Type == AssetType::SkeletalMesh) SetMesh(name);
		else if (entry->Type == AssetType::Material) SetMaterial(name);
		else LOG_ENG_WARN_F("CMeshRef::operator= - asset '%s' is not a mesh or material (type: %s)", name.GetStr(), AssetTypeName(entry->Type));

		return *this;
	}
};

TNX_REGISTER_COMPONENT(CMeshRef)
