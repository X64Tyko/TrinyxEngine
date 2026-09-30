#pragma once
#include "AssetRegistry.h"
#include "AssetTypes.h"
#include "ComponentView.h"
#include "SchemaReflector.h"

// CSkeletonRef — skeletal binding for an entity.
// Volatile: set at spawn, rarely changes, no rollback needed.
// Render group: SkeletonID drives GPU skinning. Vertex + skin weight data
// come from CMeshRef.MeshID on the same entity (skin and mesh are combined).
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct CSkeletonRef : ComponentView<CSkeletonRef, WIDTH>
{
	TNX_VOLATILE_FIELDS(CSkeletonRef, Render, SkeletonID)

	UIntProxy<WIDTH> SkeletonID{}; // slot in SkeletonManager (0 = none)

	static constexpr auto FieldRefTypes = std::array{
		AssetType::Skeleton, // SkeletonID
	};

	void SetSkeleton(TnxName name)
		requires(WIDTH == FieldWidth::Scalar)
	{
		OnSkelLoad.Bind<CSkeletonRef, &CSkeletonRef::SetSkeletonID>(this);
		OnSkelEvict.Bind<CSkeletonRef, &CSkeletonRef::ResetSkeletonID>(this);
		AssetRegistry::RegisterPendingCheckout<AssetType::Skeleton>(name, OnSkelLoad, OnSkelEvict);
	}

private:
	AssetLoad OnSkelLoad;
	AssetEvict OnSkelEvict;
	void SetSkeletonID(uint32_t newID)
		requires(WIDTH == FieldWidth::Scalar)
	{
		SkeletonID = newID;
		OnSkelLoad.Reset();
	}
	void ResetSkeletonID()
		requires(WIDTH == FieldWidth::Scalar)
	{
		SkeletonID = 0u;
		OnSkelEvict.Reset();
	}
};

TNX_REGISTER_COMPONENT(CSkeletonRef)
