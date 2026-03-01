#pragma once

#include "Registry.h"
#include "TemporalFlags.h"

//#define SFlags this->Flags.Flags

// Global counter (hidden in cpp)
namespace Internal
{
	extern uint32_t g_GlobalComponentCounter;
	extern ClassID g_GlobalClassCounter;
	// TODO: if the user changes the "Generation" bits for the Entity ID and has more than... 2B classes... nvm
}

template <template <FieldWidth> class Derived, FieldWidth WIDTH = FieldWidth::Scalar>
class EntityView
{
public:
	Registry* Reg      = nullptr;
	EntityID ID        = {};
	uint32_t ViewIndex = 0;

	TemporalFlags<WIDTH> Flags;

	static ClassID StaticClassID()
	{
		static ClassID id = Internal::g_GlobalClassCounter++;
		return id;
	}

	static constexpr auto DefineSchema()
	{
		return Schema::Create(&EntityView::Flags);
	}

protected:
	EntityView() = default;

public:
	// Delete Copy/Move to prevent "Orphaned Views"
	EntityView(const EntityView&)            = delete;
	EntityView& operator=(const EntityView&) = delete;

	static Derived<WIDTH> Get(Registry& reg, EntityID id)
	{
		Derived<WIDTH> view;
		view.Reg = &reg;
		view.ID  = id;

		return view;
	}

    FORCE_INLINE void Advance(uint32_t step)
	{
		ViewIndex += step;
		Flags.Advance(step);
	}

    FORCE_INLINE void Hydrate(void** fieldArrayTable, uint32_t index = 0, int32_t count = -1)
	{
		constexpr auto schema = Derived<WIDTH>::DefineSchema();

		size_t fieldArrayBaseIndex = 0;

		std::apply([&](auto&&... members)
		{
			(..., [&](auto member)
			{
				if constexpr (std::is_member_object_pointer_v<decltype(member)>)
				{
					using MemberType = std::remove_reference_t<decltype(static_cast<Derived<WIDTH>*>(this)->*member)>;
					// Check if this is a FieldProxy<T>
					if constexpr (HasDefineFields<MemberType>)
					{
						(static_cast<Derived<WIDTH>*>(this)->*member).Bind(&fieldArrayTable[fieldArrayBaseIndex], index, count);

						// Advance by number of fields for this component
						constexpr size_t fieldCount = MemberType::FieldNames.size();
						fieldArrayBaseIndex         += fieldCount * 2;
					}
					else
					{
						// TODO: support non-FieldProxy<T> members?
					}
				}
			}(members));
		}, schema.members);
	}
};

template <template <FieldWidth> class CLASS, template <typename, FieldWidth> class SUPER = EntityView, FieldWidth WIDTH = FieldWidth::Scalar>
using InheritableBase = SUPER<CLASS<WIDTH>, WIDTH>;

template <typename CLASS, template <typename, FieldWidth> class SUPER = EntityView, FieldWidth WIDTH = FieldWidth::Scalar>
using FinalBase = SUPER<CLASS, WIDTH>;