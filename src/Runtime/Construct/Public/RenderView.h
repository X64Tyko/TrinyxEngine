#pragma once

#include "ConstructView.h"
#include "TransRot.h"
#include "Scale.h"
#include "ColorData.h"
#include "MeshRef.h"

// ---------------------------------------------------------------------------
// RenderViewEntity — Internal ECS entity backing RenderView.
// TransRot + Scale + ColorData + MeshRef. Partition: RENDER (no physics).
// ---------------------------------------------------------------------------
template <FieldWidth WIDTH = FieldWidth::Scalar>
class RenderViewEntity : public EntityView<RenderViewEntity, WIDTH>
{
	TNX_REGISTER_SCHEMA(RenderViewEntity, EntityView, transform, scale, color, mesh)

public:
	TransRot<WIDTH> transform;
	Scale<WIDTH> scale;
	ColorData<WIDTH> color;
	MeshRef<WIDTH> mesh;
};

TNX_REGISTER_ENTITY(RenderViewEntity)

// ---------------------------------------------------------------------------
// RenderView — Construct View with Transform + Scale + Color + Mesh.
// For visual-only objects: decals, UI anchors, particle emitters.
// ---------------------------------------------------------------------------
class RenderView : public ConstructView<RenderView, RenderViewEntity>
{
public:
	template <template <FieldWidth> class Component>
	Component<FieldWidth::Scalar>& Get()
	{
		return GetComponent<Component>(GetView());
	}

	template <template <FieldWidth> class Component>
	const Component<FieldWidth::Scalar>& Get() const
	{
		return GetComponent<Component>(GetView());
	}

private:
	template <template <FieldWidth> class C>
	static auto& GetComponent(auto& view)
	{
		if constexpr (std::is_same_v<C<FieldWidth::Scalar>, TransRot<FieldWidth::Scalar>>) return view.transform;
		else if constexpr (std::is_same_v<C<FieldWidth::Scalar>, Scale<FieldWidth::Scalar>>) return view.scale;
		else if constexpr (std::is_same_v<C<FieldWidth::Scalar>, ColorData<FieldWidth::Scalar>>) return view.color;
		else if constexpr (std::is_same_v<C<FieldWidth::Scalar>, MeshRef<FieldWidth::Scalar>>) return view.mesh;
		else static_assert(sizeof(C<FieldWidth::Scalar>) == 0, "Component not present on RenderView");
	}
};