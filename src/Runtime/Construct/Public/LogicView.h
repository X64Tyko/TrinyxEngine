#pragma once

#include "ConstructView.h"
#include "TransRot.h"

// ---------------------------------------------------------------------------
// LogicViewEntity — Internal ECS entity backing LogicView.
// TransRot only. Partition: LOGIC (no physics, no rendering).
// ---------------------------------------------------------------------------
template <FieldWidth WIDTH = FieldWidth::Scalar>
class LogicViewEntity : public EntityView<LogicViewEntity, WIDTH>
{
	TNX_REGISTER_SCHEMA(LogicViewEntity, EntityView, transform)

public:
	TransRot<WIDTH> transform;
};

TNX_REGISTER_ENTITY(LogicViewEntity)

// ---------------------------------------------------------------------------
// LogicView — Construct View with Transform only.
// For pure-logic Constructs that need a position but no physics or rendering:
// spawn points, AI nav anchors, game mode markers.
// ---------------------------------------------------------------------------
class LogicView : public ConstructView<LogicView, LogicViewEntity>
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
		else static_assert(sizeof(C<FieldWidth::Scalar>) == 0, "Component not present on LogicView");
	}
};