#pragma once
#include <array>
#include <tuple>
#include <type_traits>

#include "FieldMeta.h"
#include "MemoryDefines.h"
#include "Schema.h"
#include "SchemaValidation.h"

// Platform-specific attribute to prevent linker from stripping unused symbols
#if defined(__GNUC__) || defined(__clang__)
#define STRIGID_USED_ATTR __attribute__((used))
#else
#define STRIGID_USED_ATTR
#endif

// --- TYPE TRAITS ---
template <typename T>
struct StripClass;

// Strips T Class::* down to T
template <typename C, typename M>
struct StripClass<M C::*>
{
	using Type = M;
};

template <typename Class>
struct PrefabReflector
{
	// --- 1. REGISTRATION (Static Init) ---
	static bool Register()
	{
		// Compile-time validation of entity type
		VALIDATE_ENTITY_HAS_SCHEMA(Class);
		//VALIDATE_ENTITY_IS_STANDARD_LAYOUT(Class);

		MetaRegistry::Get().RegisterPrefab<Class>();

		constexpr auto schema = Class::DefineSchema();

		// Register Components by iterating the tuple
		std::apply([](auto... args)
		{
			(ProcessSchemaItem(args), ...);
		}, schema.members);

		return true;
	}

	template <typename T, typename BaseClass>
	static void ProcessSchemaItem(T BaseClass::* memberPtr)
	{
		// Register component members (T)
		if constexpr (std::is_member_object_pointer_v<decltype(memberPtr)>)
		{
			using CompType = StripClass<decltype(memberPtr)>::Type;


			// Compile-time validation of component type
			VALIDATE_COMPONENT_IS_POD(CompType);

			// Register with the derived class (Class), not the base class
			MetaRegistry::Get().RegisterPrefabComponent<Class, CompType>();
		}
	}
};

// Helper to register fields at runtime
template <typename Derived, size_t... Is>
static void RegisterFieldsImpl(std::index_sequence<Is...>)
{
	constexpr auto fields = Derived::DefineFields();
	std::vector<FieldMeta> fieldMetas;
	fieldMetas.reserve(sizeof...(Is));

	// Extract field metadata for each member pointer
	(..., fieldMetas.push_back(ExtractFieldMeta<Derived, Is>(std::get < Is > (fields))));

	// Register with global registry
	ComponentTypeID typeID = GetComponentTypeID<Derived>();
	bool bIsTemporal       = false;
	if constexpr (requires { Derived::bTemporalComp; })
	{
		bIsTemporal = Derived::bTemporalComp;
	}
	ComponentFieldRegistry::Get().RegisterFields(typeID, std::move(fieldMetas), bIsTemporal);
}

// Extract metadata from a member pointer
template <typename Derived, size_t Index, template <typename, FieldWidth> class FieldType, typename Type, FieldWidth WIDTH>
static FieldMeta ExtractFieldMeta(FieldType<Type, WIDTH> Derived::* member)
{
	// Create temporary to get offset
	Derived temp{};
	size_t offset = reinterpret_cast<size_t>(&(temp.*member)) - reinterpret_cast<size_t>(&temp);

	const char* name = (Index < Derived::FieldNames.size()) ? Derived::FieldNames[Index] : "unknown";

	return FieldMeta{
		sizeof(Type),
		FIELD_ARRAY_ALIGNMENT, // 32 or 64 bytes depending on CMake option
		offset,
		0, // OffsetInChunk computed later by Archetype::BuildLayout
		name
	};
}

// Get compile-time field list
template <typename Derived>
static constexpr auto GetFieldPointers()
{
	return Derived::DefineFields();
}

// Get runtime field count
template <typename Derived>
static constexpr size_t GetFieldCount()
{
	return std::tuple_size_v<decltype(Derived::DefineFields())>;
}

// Static registration - called once during static initialization
template <typename Derived>
static bool RegisterFieldsStatic()
{
	RegisterFieldsImpl<Derived>(std::make_index_sequence < GetFieldCount<Derived>() >
	{
	}
	)
	;
	return true;
}

// Extract metadata from a member pointer
template <typename Derived, size_t Index, typename FieldType>
static FieldMeta ExtractFieldMeta(FieldType Derived::* member)
{
	// Create temporary to get offset
	Derived temp{};
	size_t offset = reinterpret_cast<size_t>(&(temp.*member)) - reinterpret_cast<size_t>(&temp);

	const char* name = (Index < Derived::FieldNames.size()) ? Derived::FieldNames[Index] : "unknown";

	return FieldMeta{
		sizeof(FieldType),
		FIELD_ARRAY_ALIGNMENT, // 32 or 64 bytes depending on CMake option
		offset,
		0, // OffsetInChunk computed later by Archetype::BuildLayout
		name
	};
}

// Helper: Apply function to each field
template <typename T, typename Func>
FORCE_INLINE void ForEachField(Func&& func)
{
	constexpr auto fields = T::DefineFields();
	[&]<size_t... Is>(std::index_sequence<Is...>)
	{
		(func(std::get < Is > (fields), Is), ...);
	}(std::make_index_sequence<std::tuple_size_v<decltype(fields)>>{});
}

// --- MACRO ---
#define STRIGID_REGISTER_ENTITY(CLASS) \
    namespace { \
        static const bool g_Reflect_##CLASS = []() { \
            PrefabReflector<CLASS<>>::Register(); \
            return true; \
        }(); \
    }
#define STRIGID_REGISTER_SCHEMA(CLASS, SUPER, ...) \
    public: \
    static constexpr auto DefineSchema() \
    { \
        return SUPER<CLASS, WIDTH>::DefineSchema().Extend(__VA_OPT__(STRIGID_MAP_LIST(STRIGID_GET_PTR, CLASS, __VA_ARGS__))); \
    } \
    \
    FORCE_INLINE void Advance(uint32_t step) \
    { \
        SUPER<CLASS, WIDTH>::Advance(step); \
        __VA_OPT__(STRIGID_MAPF_LIST(STRIGID_BIND_ADVANCE, CLASS, __VA_ARGS__)) \
    } \
    \
    using Base = SUPER<CLASS, WIDTH>; \
    using WideType = CLASS<FieldWidth::Wide>; \
    using MaskedType = CLASS<FieldWidth::WideMask>; \
    \
    private: \
    struct _EntityRegistrar { \
        _EntityRegistrar() { PrefabReflector<CLASS<>>::Register(); } \
    }; \
    [[maybe_unused]] STRIGID_USED_ATTR static inline _EntityRegistrar _entity_registered;

#define STRIGID_REGISTER_SUPER_SCHEMA(CLASS, SUPER, ...) \
    public: \
    static constexpr auto DefineSchema() \
    { \
        return SUPER<T, WIDTH>::DefineSchema().Extend(__VA_OPT__(STRIGID_MAP_LIST(STRIGID_GET_PTR, CLASS, __VA_ARGS__))); \
    } \
    \
    FORCE_INLINE void Advance(uint32_t step) \
    { \
        SUPER<T, WIDTH>::Advance(step); \
        __VA_OPT__(STRIGID_MAPF_LIST(STRIGID_BIND_ADVANCE, CLASS, __VA_ARGS__)) \
    }
#define STRIGID_EXPAND(x) x
#define STRIGID_GET_ARG_COUNT(...) STRIGID_EXPAND(STRIGID_INTERNAL_ARG_COUNT(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1))
#define STRIGID_INTERNAL_ARG_COUNT(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16, count, ...) count

// Mapping Dispatcher
#define STRIGID_MAP_LIST(m, context, ...) STRIGID_EXPAND(STRIGID_CONCAT(STRIGID_MAP_, STRIGID_GET_ARG_COUNT(__VA_ARGS__))(m, context, __VA_ARGS__))
#define STRIGID_MAPF_LIST(m, context, ...) STRIGID_EXPAND(STRIGID_CONCAT(STRIGID_MAPF_, STRIGID_GET_ARG_COUNT(__VA_ARGS__))(m, context, __VA_ARGS__))
#define STRIGID_CONCAT_INNER(a, b) a##b
#define STRIGID_CONCAT(a, b) STRIGID_CONCAT_INNER(a, b)

// Individual Expansion Steps (Supports up to 16 members per component)
#define STRIGID_MAP_1(m, c, x)      m(c, x)
#define STRIGID_MAP_2(m, c, x, ...) m(c, x), STRIGID_MAP_1(m, c, __VA_ARGS__)
#define STRIGID_MAP_3(m, c, x, ...) m(c, x), STRIGID_MAP_2(m, c, __VA_ARGS__)
#define STRIGID_MAP_4(m, c, x, ...) m(c, x), STRIGID_MAP_3(m, c, __VA_ARGS__)
#define STRIGID_MAP_5(m, c, x, ...) m(c, x), STRIGID_MAP_4(m, c, __VA_ARGS__)
#define STRIGID_MAP_6(m, c, x, ...) m(c, x), STRIGID_MAP_5(m, c, __VA_ARGS__)
#define STRIGID_MAP_7(m, c, x, ...) m(c, x), STRIGID_MAP_6(m, c, __VA_ARGS__)
#define STRIGID_MAP_8(m, c, x, ...) m(c, x), STRIGID_MAP_7(m, c, __VA_ARGS__)
#define STRIGID_MAP_9(m, c, x, ...) m(c, x), STRIGID_MAP_8(m, c, __VA_ARGS__)
#define STRIGID_MAP_10(m, c, x, ...) m(c, x), STRIGID_MAP_9(m, c, __VA_ARGS__)
#define STRIGID_MAP_11(m, c, x, ...) m(c, x), STRIGID_MAP_10(m, c, __VA_ARGS__)
#define STRIGID_MAP_12(m, c, x, ...) m(c, x), STRIGID_MAP_11(m, c, __VA_ARGS__)
#define STRIGID_MAP_13(m, c, x, ...) m(c, x), STRIGID_MAP_12(m, c, __VA_ARGS__)
#define STRIGID_MAP_14(m, c, x, ...) m(c, x), STRIGID_MAP_13(m, c, __VA_ARGS__)
#define STRIGID_MAP_15(m, c, x, ...) m(c, x), STRIGID_MAP_14(m, c, __VA_ARGS__)
#define STRIGID_MAP_16(m, c, x, ...) m(c, x), STRIGID_MAP_15(m, c, __VA_ARGS__)

#define STRIGID_MAPF_1(m, c, x)      m(c, x)
#define STRIGID_MAPF_2(m, c, x, ...) m(c, x) STRIGID_MAP_1(m, c, __VA_ARGS__)
#define STRIGID_MAPF_3(m, c, x, ...) m(c, x) STRIGID_MAPF_2(m, c, __VA_ARGS__)
#define STRIGID_MAPF_4(m, c, x, ...) m(c, x) STRIGID_MAPF_3(m, c, __VA_ARGS__)
#define STRIGID_MAPF_5(m, c, x, ...) m(c, x) STRIGID_MAPF_4(m, c, __VA_ARGS__)
#define STRIGID_MAPF_6(m, c, x, ...) m(c, x) STRIGID_MAPF_5(m, c, __VA_ARGS__)
#define STRIGID_MAPF_7(m, c, x, ...) m(c, x) STRIGID_MAPF_6(m, c, __VA_ARGS__)
#define STRIGID_MAPF_8(m, c, x, ...) m(c, x) STRIGID_MAPF_7(m, c, __VA_ARGS__)
#define STRIGID_MAPF_9(m, c, x, ...) m(c, x) STRIGID_MAPF_8(m, c, __VA_ARGS__)
#define STRIGID_MAPF_10(m, c, x, ...) m(c, x) STRIGID_MAPF_9(m, c, __VA_ARGS__)
#define STRIGID_MAPF_11(m, c, x, ...) m(c, x) STRIGID_MAPF_10(m, c, __VA_ARGS__)
#define STRIGID_MAPF_12(m, c, x, ...) m(c, x) STRIGID_MAPF_11(m, c, __VA_ARGS__)
#define STRIGID_MAPF_13(m, c, x, ...) m(c, x) STRIGID_MAPF_12(m, c, __VA_ARGS__)
#define STRIGID_MAPF_14(m, c, x, ...) m(c, x) STRIGID_MAPF_13(m, c, __VA_ARGS__)
#define STRIGID_MAPF_15(m, c, x, ...) m(c, x) STRIGID_MAPF_14(m, c, __VA_ARGS__)
#define STRIGID_MAPF_16(m, c, x, ...) m(c, x) STRIGID_MAPF_15(m, c, __VA_ARGS__)

// Macro to auto-register fields during static initialization
#define STRIGID_REGISTER_COMPONENT_FIELDS(ComponentType) \
    namespace { \
        static bool _##ComponentType##_FieldsRegistered = RegisterFieldsStatic<ComponentType>(); \
    }

// Helper to prefix the class name to the member pointer
#define STRIGID_GET_PTR(Class, Member) &Class::Member

// Helper to stringify the member name
#define STRIGID_GET_NAME(Class, Member) #Member

#define STRIGID_BIND_ADVANCE(ComponentType, Member, ...) Member.Advance(step);

#define STRIGID_BIND_FINAL(ComponentType, Member, ...) Member.MaskFinal(count);

#define STRIGID_BIND_BIND(ComponentType, Member, ...) Member.Bind(arrays[arrayIndex], arrays[arrayIndex + 1], startIndex, count); arrayIndex += 2;

// Handles creating the field definition, debug field names, Bind function, and Registering the struct component
#define STRIGID_REGISTER_FIELDS(ComponentType, ...) \
    static constexpr auto DefineFields() \
    { /* Use the map bindings and __VA_OPT__ to create our Field Definitions for each item passed to the macro */ \
        return std::make_tuple(__VA_OPT__(STRIGID_MAP_LIST(STRIGID_GET_PTR, ComponentType, __VA_ARGS__))); \
    } \
    \
    static constexpr auto FieldNames = std::array{ /* Same thing but for the names of the fields */ \
        __VA_OPT__(STRIGID_MAP_LIST(STRIGID_GET_NAME, ComponentType, __VA_ARGS__)) \
    }; \
\
    FORCE_INLINE void Advance(uint32_t step) \
    { \
        __VA_OPT__(STRIGID_MAPF_LIST(STRIGID_BIND_ADVANCE, ComponentType, __VA_ARGS__)) \
    } \
\
    FORCE_INLINE void Bind(void** arrays, uint32_t startIndex = 0, int32_t count = -1) \
    { \
        int32_t arrayIndex = 0; \
        __VA_OPT__(STRIGID_MAPF_LIST(STRIGID_BIND_BIND, ComponentType, __VA_ARGS__)) \
    }
#define STRIGID_REGISTER_COMPONENT(ComponentType) \
    namespace { \
        struct _##ComponentType##_Registrar { \
            _##ComponentType##_Registrar() { \
                RegisterFieldsStatic<ComponentType<>>(); \
            } \
        }; \
        [[maybe_unused]] STRIGID_USED_ATTR static _##ComponentType##_Registrar _##ComponentType##_FieldsRegistered; \
    }
#define STRIGID_TEMPORAL_FIELDS(ComponentType, ...) \
    static inline bool bTemporalComp = true; \
    STRIGID_REGISTER_FIELDS(ComponentType, __VA_ARGS__)