#pragma once
#include <functional>
#include <string_view>
#include <tuple>

#include "ReflectionRegistry.h"

#define REGISTER_ENTITY_PREPHYS(Type, ClassID) \
    case ClassID: InvokePrePhysicsImpl<Type>(dt, fieldArrayTable, componentCount); break;

// The container for member pointers
template <typename... Members>
struct SchemaDefinition
{
	std::tuple<Members...> members;

	constexpr SchemaDefinition(Members... m)
		: members(m...)
	{
	}

	// EXTEND: Allows derived classes to append their own members
	template <typename... NewMembers>
	constexpr auto Extend(NewMembers... newMembers) const
	{
		return std::apply([&](auto... currentMembers)
		{
			return SchemaDefinition<Members..., NewMembers...>(currentMembers..., newMembers...);
		}, members);
	}

	template <typename Target, typename Replacement>
	constexpr auto Replace(Target target, Replacement replacement) const
	{
		// Unpack the existing tuple...
		return std::apply([&](auto... args)
		{
			// ...and rebuild a NEW SchemaDefinition
			return SchemaDefinition<decltype(ResolveReplacement(args, target, replacement))...>(
				ResolveReplacement(args, target, replacement)...
			);
		}, members);
	}

private:
	// Helper: Selects either the 'current' item or the 'replacement'
	// based on whether 'current' matches the 'target' we want to remove.
	template <typename Current, typename Target, typename Replacement>
	static constexpr auto ResolveReplacement(Current current, Target target, Replacement replacement)
	{
		if constexpr (std::is_same_v<Current, Target>)
		{
			return (current == target) ? replacement : current;
		}
		else
		{
			return current;
		}
	}
};

// The static builder interface
struct Schema
{
	template <typename... Args>
	static constexpr auto Create(Args... args)
	{
		return SchemaDefinition<Args...>(args...);
	}
};
