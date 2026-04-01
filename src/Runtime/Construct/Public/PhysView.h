#pragma once

#include "ConstructView.h"
#include "TransRot.h"
#include "JoltBody.h"

// ---------------------------------------------------------------------------
// PhysViewEntity — Internal ECS entity backing PhysView.
// TransRot + JoltBody only. Partition: PHYS (physics, no rendering).
// ---------------------------------------------------------------------------
template <FieldWidth WIDTH = FieldWidth::Scalar>
class PhysViewEntity : public EntityView<PhysViewEntity, WIDTH>
{
	TNX_REGISTER_SCHEMA(PhysViewEntity, EntityView, transform, physBody)

public:
	TransRot<WIDTH> transform;
	JoltBody<WIDTH> physBody;
};

TNX_REGISTER_ENTITY(PhysViewEntity)

// ---------------------------------------------------------------------------
// PhysView — Construct View with Transform + Physics only.
// For invisible physics objects: triggers, zones, force volumes.
// ---------------------------------------------------------------------------
class PhysView : public ConstructView<PhysView, PhysViewEntity>
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
		else if constexpr (std::is_same_v<C<FieldWidth::Scalar>, JoltBody<FieldWidth::Scalar>>) return view.physBody;
		else static_assert(sizeof(C<FieldWidth::Scalar>) == 0, "Component not present on PhysView");
	}
};