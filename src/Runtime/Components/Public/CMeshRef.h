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
		AssetType::Mesh,     // MeshID
		AssetType::Material, // MaterialID
		AssetType::Invalid,  // LODCount
		AssetType::Invalid,  // CastShadow
	};

	TNX_VOLATILE_FIELDS(CMeshRef, Render, MeshID, MaterialID, LODCount, CastShadow)

	// --- Init-lambda assignment API (Scalar only) ---

	// Checks out MeshID and, if the entry has a DefaultMaterial and MaterialID is unset, a MaterialID checkout.
	void SetMesh(TnxName name)
		requires(WIDTH == FieldWidth::Scalar)
	{
		const AssetEntry* entry = AssetRegistry::Get().FindByTNameAndType(name, AssetType::Mesh);
		if (!entry)
		{
			LOG_ENG_WARN_F("CMeshRef::SetMesh - mesh '%s' not found in asset registry", name.GetStr());
			return;
		}
		OnMeshLoad.Bind<CMeshRef, &CMeshRef::SetMeshID>(this);
		OnMeshEvict.Bind<CMeshRef, &CMeshRef::ResetMeshID>(this);
		AssetRegistry::RegisterPendingCheckout<AssetType::Mesh>(name, OnMeshLoad, OnMeshEvict);

		if (entry->DefaultMaterial.Value != 0 && MaterialID.Value() == 0)
		{
			OnMatLoad.Bind<CMeshRef, &CMeshRef::SetMaterialID>(this);
			OnMatEvict.Bind<CMeshRef, &CMeshRef::ResetMaterialID>(this);
			AssetRegistry::RegisterPendingCheckout<AssetType::Material>(entry->DefaultMaterial, OnMatLoad, OnMatEvict);
		}
	}

	// Checks out MaterialID only.
	void SetMaterial(TnxName name)
		requires(WIDTH == FieldWidth::Scalar)
	{
		OnMatLoad.Bind<CMeshRef, &CMeshRef::SetMaterialID>(this);
		OnMatEvict.Bind<CMeshRef, &CMeshRef::ResetMaterialID>(this);
		AssetRegistry::RegisterPendingCheckout<AssetType::Material>(name, OnMatLoad, OnMatEvict);
	}

	// Dispatches to SetMesh or SetMaterial based on the catalogued asset type.
	CMeshRef& operator=(TnxName name)
		requires(WIDTH == FieldWidth::Scalar)
	{
		auto entries = AssetRegistry::Get().GetAssetsByName(name);
		for (const AssetEntry* entry : entries)
		{
			if (entry->Type == AssetType::Mesh)
			{
				SetMesh(name);
				return *this;
			}
			if (entry->Type == AssetType::Material)
			{
				SetMaterial(name);
				return *this;
			}
		}
		LOG_ENG_WARN_F("CMeshRef::operator= - no mesh or material named '%s' found", name.GetStr());
		return *this;
	}

private:
	AssetLoad OnMeshLoad;
	AssetEvict OnMeshEvict;
	AssetLoad OnMatLoad;
	AssetEvict OnMatEvict;

	void SetMeshID(uint32_t id)
		requires(WIDTH == FieldWidth::Scalar)
	{
		MeshID = id;
		OnMeshLoad.Reset();
	}
	void ResetMeshID()
		requires(WIDTH == FieldWidth::Scalar)
	{
		MeshID = 0u;
		OnMeshEvict.Reset();
	}
	void SetMaterialID(uint32_t id)
		requires(WIDTH == FieldWidth::Scalar)
	{
		MaterialID = id;
		OnMatLoad.Reset();
	}
	void ResetMaterialID()
		requires(WIDTH == FieldWidth::Scalar)
	{
		MaterialID = 0u;
		OnMatEvict.Reset();
	}
};

TNX_REGISTER_COMPONENT(CMeshRef)
