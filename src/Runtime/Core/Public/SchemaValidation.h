#pragma once
#include <type_traits>

// Schema Validation - Provides better compile-time error messages for common mistakes

template <typename FieldType, FieldWidth WIDTH>
struct FieldProxy;

namespace SchemaValidation
{
	// Check if a type has a DefineSchema() method
	template <typename T, typename = void>
	struct HasDefineSchema : std::false_type
	{
	};

	template <typename T>
	struct HasDefineSchema<T, std::void_t<decltype(T::DefineSchema())>> : std::true_type
	{
	};

	// Type trait helper
	template <typename T>
	struct IsFieldProxy : std::false_type
	{
	};

	template <typename T, FieldWidth WIDTH>
	struct IsFieldProxy<FieldProxy<T, WIDTH>> : std::true_type
	{
	};

	// Extract the member type from a pointer-to-member: &T::x -> decltype(T::x)
	template <typename>
	struct MemberType;

	template <typename C, typename M>
	struct MemberType<M C::*>
	{
		using Type = M;
	};

	// Check that every element in a tuple of member pointers points to a FieldProxy
	template <typename Tuple, size_t... Is>
	constexpr bool AllFieldProxyImpl(std::index_sequence<Is...>)
	{
		return (IsFieldProxy<typename MemberType<
			std::tuple_element_t<Is, Tuple>>::Type>::value && ...);
	}

	template <typename Tuple>
	constexpr bool AllFieldProxyCheck()
	{
		return AllFieldProxyImpl<Tuple>(
			std::make_index_sequence<std::tuple_size_v<Tuple>>{});
	}

	// Check that every element in a tuple of member pointers is trivially copyable
	template <typename Tuple, size_t... Is>
	constexpr bool AllTriviallyCopyableImpl(std::index_sequence<Is...>)
	{
		return (std::is_trivially_copyable_v<typename MemberType<
			std::tuple_element_t<Is, Tuple>>::Type> && ...);
	}

	template <typename Tuple>
	constexpr bool AllTriviallyCopyableCheck()
	{
		return AllTriviallyCopyableImpl<Tuple>(
			std::make_index_sequence<std::tuple_size_v<Tuple>>{});
	}

	// Helper to check if all fields of a component are valid:
	// - Temporal/Volatile components: all fields must be FieldProxy
	// - Cold components: all fields must be trivially copyable
	template <typename T>
	struct AllFieldsAreFieldProxy
	{
		template <typename U>
		static auto test(int) -> decltype(
			std::declval<U>().DefineFields(),
			std::true_type{}
		);

		template <typename>
		static std::false_type test(...);

		static constexpr bool has_fields = decltype(test<T>(0))::value;

		static constexpr bool value = []()
		{
			if constexpr (!has_fields) return false;
			return AllFieldProxyCheck<decltype(T::DefineFields())>();
		}();
	};

	// All components use FieldProxy — they are views into SoA arrays, not
	// data containers. Two hard constraints:
	//   1. No virtual functions (vtable breaks SoA decomposition)
	//   2. All data fields must be FieldProxy (raw floats/ints would bypass SoA)
	// Components may contain accessor structs (e.g. RotationAccessor with
	// reference members) — these are views, not stored data.
	template <typename T>
	struct IsValidComponent
	{
		static constexpr bool value =
			!std::is_polymorphic_v<T> &&
			AllFieldsAreFieldProxy<T>::value;
	};
} // namespace SchemaValidation

// Helpful error message macros with better formatting
#define VALIDATE_ENTITY_HAS_SCHEMA(Type) \
    static_assert(SchemaValidation::HasDefineSchema<Type>::value, \
        "\n\n" \
        "================================================================\n" \
        "ERROR: Entity missing DefineSchema()!\n" \
        "================================================================\n" \
        "\n" \
        "Add this to your entity class:\n" \
        "\n" \
        "    static constexpr auto DefineSchema() {\n" \
        "        return Schema::Create(\n" \
        "            &YourEntity::component1,\n" \
        "            &YourEntity::Update\n" \
        "        );\n" \
        "    }\n" \
        "\n" \
        "================================================================\n")

#define VALIDATE_ENTITY_IS_STANDARD_LAYOUT(Type) \
    static_assert(std::is_standard_layout_v<Type>, \
        "\n\n" \
        "================================================================\n" \
        "ERROR: Entity must be standard layout!\n" \
        "================================================================\n" \
        "\n" \
        "Entity types cannot have:\n" \
        "  - Virtual functions\n" \
        "  - Complex inheritance\n" \
        "\n" \
        "Entities are lightweight data containers.\n" \
        "================================================================\n")

#define VALIDATE_COMPONENT_IS_POD(Type) \
    static_assert(SchemaValidation::IsValidComponent<Type>::value, \
        "\n\n" \
        "================================================================\n" \
        "ERROR: Component '" #Type "' must be POD (plain old data)!\n" \
        "================================================================\n" \
        "\n" \
        "Component: " #Type "\n" \
        "\n" \
        "Components CANNOT have:\n" \
        "  - Virtual functions\n" \
        "  - std::string, std::vector, or complex types\n" \
        "  - Heap-allocated pointers\n" \
        "\n" \
        "Use simple structs with raw data only:\n" \
        "\n" \
        "    struct Transform {\n" \
        "        FloatProxy<WIDTH> PositionX, PositionY, PositionZ;\n" \
        "        FloatProxy<WIDTH> RotationX, RotationY, RotationZ;\n" \
        "    };\n" \
        "\n" \
        "================================================================\n")
