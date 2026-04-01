#pragma once

#include "ConstructView.h"
#include "TransRot.h"
#include "JoltBody.h"
#include "Scale.h"
#include "ColorData.h"
#include "MeshRef.h"

// ---------------------------------------------------------------------------
// DefaultViewEntity — Internal ECS entity backing DefaultView.
//
// This is a regular entity: it participates in wide sweeps, physics, rendering,
// and replication. All Constructs that use DefaultView share this archetype.
// Partition: DUAL (has both Physics and Render components).
// ---------------------------------------------------------------------------
template <FieldWidth WIDTH = FieldWidth::Scalar>
class DefaultViewEntity : public EntityView<DefaultViewEntity, WIDTH>
{
	TNX_REGISTER_SCHEMA(DefaultViewEntity, EntityView, transform, physBody, scale, color, mesh)

public:
	TransRot<WIDTH> transform;
	JoltBody<WIDTH> physBody;
	Scale<WIDTH> scale;
	ColorData<WIDTH> color;
	MeshRef<WIDTH> mesh;
};

TNX_REGISTER_ENTITY(DefaultViewEntity)

// ---------------------------------------------------------------------------
// DefaultView — Construct View with Transform + Physics + Scale + Color + Mesh.
//
// The standard "full" view for Constructs that exist physically in the world
// with a visible mesh and a physics body. All DefaultView users share the same
// archetype — adding game-specific state belongs on the Construct, not here.
//
// Usage:
//   class Player : public Construct<Player>
//   {
//       DefaultView Body;
//
//       void InitializeViews()
//       {
//           Body.Initialize(GetRegistry());
//           Body.Get<TransRot>().PosY = 5.0f;
//           Body.Get<JoltBody>().Shape = JoltShapeType::Box;
//       }
//   };
// ---------------------------------------------------------------------------
class DefaultView : public ConstructView<DefaultView, DefaultViewEntity>
{
public:
	// Type-safe component access. Returns a reference to the scalar component.
	// Compile-time error if the requested component doesn't exist on this view.
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
	// Resolve component by type — each overload matches one member.
	template <template <FieldWidth> class C>
	static auto& GetComponent(auto& view)
	{
		if constexpr (std::is_same_v<C<FieldWidth::Scalar>, TransRot<FieldWidth::Scalar>>) return view.transform;
		else if constexpr (std::is_same_v<C<FieldWidth::Scalar>, JoltBody<FieldWidth::Scalar>>) return view.physBody;
		else if constexpr (std::is_same_v<C<FieldWidth::Scalar>, Scale<FieldWidth::Scalar>>) return view.scale;
		else if constexpr (std::is_same_v<C<FieldWidth::Scalar>, ColorData<FieldWidth::Scalar>>) return view.color;
		else if constexpr (std::is_same_v<C<FieldWidth::Scalar>, MeshRef<FieldWidth::Scalar>>) return view.mesh;
		else static_assert(sizeof(C<FieldWidth::Scalar>) == 0, "Component not present on DefaultView");
	}
};
