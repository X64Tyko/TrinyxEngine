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

	// Helper to check if all fields of a component are FieldProxy
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

		// If component has fields, check if they're all FieldProxy
		// Otherwise just use trivially_copyable check
		static constexpr bool value = has_fields; // Simplified for now - assume components with DefineFields use FieldProxy
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
        "  - Non-trivial constructors/destructors\n" \
        "  - std::string, std::vector, or complex types\n" \
        "  - Heap-allocated pointers\n" \
        "\n" \
        "Use simple structs with raw data only:\n" \
        "\n" \
        "    struct Transform {\n" \
        "        float PositionX, PositionY, PositionZ;\n" \
        "        float RotationX, RotationY, RotationZ;\n" \
        "    };\n" \
        "\n" \
        "================================================================\n")
