#pragma once
#include <array>
#include <cassert>
#include <tuple>
#include <type_traits>

#include "FieldMeta.h"
#include "MemoryDefines.h"
#include "Schema.h"
#include "SchemaValidation.h"

// Platform-specific attribute to prevent linker from stripping unused symbols
#if defined(__GNUC__) || defined(__clang__)
#define TNX_USED_ATTR __attribute__((used))
#else
#define TNX_USED_ATTR
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

		ReflectionRegistry::Get().RegisterPrefab<Class>();

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
			ReflectionRegistry::Get().RegisterPrefabComponent<Class, CompType>();
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
	ComponentTypeID typeID = Derived::StaticTypeID();
	CacheTier Tier         = CacheTier::None;
	if constexpr (requires { Derived::TemporalTier; })
	{
		Tier = Derived::TemporalTier;
#ifndef TNX_ENABLE_ROLLBACK
		if (Tier == CacheTier::Temporal) Tier = CacheTier::Volatile;
#endif
	}
	ReflectionRegistry::Get().RegisterFields(typeID, Derived::ComponentTypeName, std::move(fieldMetas), Tier, Derived::StaticTemporalIndex());
}

// Compile-time C++ type → FieldValueType mapping.
template <typename T>
constexpr FieldValueType DeduceFieldValueType()
{
	if constexpr (std::is_same_v<T, float>) return FieldValueType::Float32;
	else if constexpr (std::is_same_v<T, double>) return FieldValueType::Float64;
	else if constexpr (std::is_same_v<T, int32_t>) return FieldValueType::Int32;
	else if constexpr (std::is_same_v<T, uint32_t>) return FieldValueType::Uint32;
	else if constexpr (std::is_same_v<T, int64_t>) return FieldValueType::Int64;
	else if constexpr (std::is_same_v<T, uint64_t>) return FieldValueType::Uint64;
	else return FieldValueType::Unknown;
}

// Extract metadata from a member pointer
template <typename Derived, size_t Index, template <typename, FieldWidth> class FieldType, typename Type, FieldWidth WIDTH>
static FieldMeta ExtractFieldMeta(FieldType<Type, WIDTH> Derived::* member)
{
	// Create temporary to get offset
	Derived temp{};
	size_t offset = reinterpret_cast<size_t>(&(temp.*member)) - reinterpret_cast<size_t>(&temp);

	const char* name = (Index < Derived::FieldNames.size()) ? Derived::FieldNames[Index] : "unknown";

	// Read asset type annotation if the component provides FieldRefTypes
	AssetType refType = AssetType::Invalid;
	if constexpr (requires { Derived::FieldRefTypes; })
	{
		if constexpr (Index < std::tuple_size_v<std::remove_cvref_t<decltype(Derived::FieldRefTypes)>>) refType = Derived::FieldRefTypes[Index];
	}

	return FieldMeta{
		sizeof(Type),
		FIELD_ARRAY_ALIGNMENT, // 32 or 64 bytes depending on CMake option
		offset,
		0, // OffsetInChunk computed later by Archetype::BuildLayout
		name,
		DeduceFieldValueType<Type>(),
		refType
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
#define TNX_REGISTER_ENTITY(CLASS) \
    namespace { \
        static const bool g_Reflect_##CLASS = []() { \
            PrefabReflector<CLASS<>>::Register(); \
            return true; \
        }(); \
    }
#define TNX_REGISTER_SCHEMA(CLASS, SUPER, ...) \
    public: \
    static constexpr const char* EntityTypeName = #CLASS; \
    static constexpr auto DefineSchema() \
    { \
        return SUPER<CLASS, WIDTH>::DefineSchema().Extend(__VA_OPT__(TNX_MAP_LIST(TNX_GET_PTR, CLASS, __VA_ARGS__))); \
    } \
    \
    FORCE_INLINE void Advance(uint32_t step) \
    { \
        SUPER<CLASS, WIDTH>::Advance(step); \
        __VA_OPT__(TNX_MAPF_LIST(TNX_BIND_ADVANCE, CLASS, __VA_ARGS__)) \
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
    [[maybe_unused]] TNX_USED_ATTR static inline _EntityRegistrar _entity_registered;

#define TNX_REGISTER_SUPER_SCHEMA(CLASS, SUPER, ...) \
    public: \
    static constexpr auto DefineSchema() \
    { \
        return SUPER<T, WIDTH>::DefineSchema().Extend(__VA_OPT__(TNX_MAP_LIST(TNX_GET_PTR, CLASS, __VA_ARGS__))); \
    } \
    \
    FORCE_INLINE void Advance(uint32_t step) \
    { \
        SUPER<T, WIDTH>::Advance(step); \
        __VA_OPT__(TNX_MAPF_LIST(TNX_BIND_ADVANCE, CLASS, __VA_ARGS__)) \
    }
#define TNX_EXPAND(x) x
#define TNX_GET_ARG_COUNT(...) TNX_EXPAND(TNX_INTERNAL_ARG_COUNT(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1))
#define TNX_INTERNAL_ARG_COUNT(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16, count, ...) count

// Mapping Dispatcher
#define TNX_MAP_LIST(m, context, ...) TNX_EXPAND(TNX_CONCAT(TNX_MAP_, TNX_GET_ARG_COUNT(__VA_ARGS__))(m, context, __VA_ARGS__))
#define TNX_MAPF_LIST(m, context, ...) TNX_EXPAND(TNX_CONCAT(TNX_MAPF_, TNX_GET_ARG_COUNT(__VA_ARGS__))(m, context, __VA_ARGS__))
#define TNX_CONCAT_INNER(a, b) a##b
#define TNX_CONCAT(a, b) TNX_CONCAT_INNER(a, b)

// Individual Expansion Steps (Supports up to 16 members per component)
#define TNX_MAP_1(m, c, x)      m(c, x)
#define TNX_MAP_2(m, c, x, ...) m(c, x), TNX_MAP_1(m, c, __VA_ARGS__)
#define TNX_MAP_3(m, c, x, ...) m(c, x), TNX_MAP_2(m, c, __VA_ARGS__)
#define TNX_MAP_4(m, c, x, ...) m(c, x), TNX_MAP_3(m, c, __VA_ARGS__)
#define TNX_MAP_5(m, c, x, ...) m(c, x), TNX_MAP_4(m, c, __VA_ARGS__)
#define TNX_MAP_6(m, c, x, ...) m(c, x), TNX_MAP_5(m, c, __VA_ARGS__)
#define TNX_MAP_7(m, c, x, ...) m(c, x), TNX_MAP_6(m, c, __VA_ARGS__)
#define TNX_MAP_8(m, c, x, ...) m(c, x), TNX_MAP_7(m, c, __VA_ARGS__)
#define TNX_MAP_9(m, c, x, ...) m(c, x), TNX_MAP_8(m, c, __VA_ARGS__)
#define TNX_MAP_10(m, c, x, ...) m(c, x), TNX_MAP_9(m, c, __VA_ARGS__)
#define TNX_MAP_11(m, c, x, ...) m(c, x), TNX_MAP_10(m, c, __VA_ARGS__)
#define TNX_MAP_12(m, c, x, ...) m(c, x), TNX_MAP_11(m, c, __VA_ARGS__)
#define TNX_MAP_13(m, c, x, ...) m(c, x), TNX_MAP_12(m, c, __VA_ARGS__)
#define TNX_MAP_14(m, c, x, ...) m(c, x), TNX_MAP_13(m, c, __VA_ARGS__)
#define TNX_MAP_15(m, c, x, ...) m(c, x), TNX_MAP_14(m, c, __VA_ARGS__)
#define TNX_MAP_16(m, c, x, ...) m(c, x), TNX_MAP_15(m, c, __VA_ARGS__)

#define TNX_MAPF_1(m, c, x)      m(c, x)
#define TNX_MAPF_2(m, c, x, ...) m(c, x) TNX_MAP_1(m, c, __VA_ARGS__)
#define TNX_MAPF_3(m, c, x, ...) m(c, x) TNX_MAPF_2(m, c, __VA_ARGS__)
#define TNX_MAPF_4(m, c, x, ...) m(c, x) TNX_MAPF_3(m, c, __VA_ARGS__)
#define TNX_MAPF_5(m, c, x, ...) m(c, x) TNX_MAPF_4(m, c, __VA_ARGS__)
#define TNX_MAPF_6(m, c, x, ...) m(c, x) TNX_MAPF_5(m, c, __VA_ARGS__)
#define TNX_MAPF_7(m, c, x, ...) m(c, x) TNX_MAPF_6(m, c, __VA_ARGS__)
#define TNX_MAPF_8(m, c, x, ...) m(c, x) TNX_MAPF_7(m, c, __VA_ARGS__)
#define TNX_MAPF_9(m, c, x, ...) m(c, x) TNX_MAPF_8(m, c, __VA_ARGS__)
#define TNX_MAPF_10(m, c, x, ...) m(c, x) TNX_MAPF_9(m, c, __VA_ARGS__)
#define TNX_MAPF_11(m, c, x, ...) m(c, x) TNX_MAPF_10(m, c, __VA_ARGS__)
#define TNX_MAPF_12(m, c, x, ...) m(c, x) TNX_MAPF_11(m, c, __VA_ARGS__)
#define TNX_MAPF_13(m, c, x, ...) m(c, x) TNX_MAPF_12(m, c, __VA_ARGS__)
#define TNX_MAPF_14(m, c, x, ...) m(c, x) TNX_MAPF_13(m, c, __VA_ARGS__)
#define TNX_MAPF_15(m, c, x, ...) m(c, x) TNX_MAPF_14(m, c, __VA_ARGS__)
#define TNX_MAPF_16(m, c, x, ...) m(c, x) TNX_MAPF_15(m, c, __VA_ARGS__)

// Macro to auto-register fields during static initialization
#define TNX_REGISTER_COMPONENT_FIELDS(ComponentType) \
    namespace { \
        static bool _##ComponentType##_FieldsRegistered = RegisterFieldsStatic<ComponentType>(); \
    }

// Helper to prefix the class name to the member pointer
#define TNX_GET_PTR(Class, Member) &Class::Member

// Helper to stringify the member name
#define TNX_GET_NAME(Class, Member) #Member

#define TNX_BIND_ADVANCE(ComponentType, Member, ...) Member.Advance(step);

#define TNX_BIND_FINAL(ComponentType, Member, ...) Member.MaskFinal(count);

#define TNX_BIND_BIND(ComponentType, Member, ...) Member.Bind(arrays[arrayIndex], flagsArray, startIndex, count); arrayIndex += 1;

// Handles creating the field definition, debug field names, Bind function, and Registering the struct component
#define TNX_REGISTER_FIELDS(ComponentType, ...) \
    static constexpr const char* ComponentTypeName = #ComponentType; \
    static constexpr auto DefineFields() \
    { /* Use the map bindings and __VA_OPT__ to create our Field Definitions for each item passed to the macro */ \
        return std::make_tuple(__VA_OPT__(TNX_MAP_LIST(TNX_GET_PTR, ComponentType, __VA_ARGS__))); \
    } \
    \
    static constexpr auto FieldNames = std::array{ /* Same thing but for the names of the fields */ \
        __VA_OPT__(TNX_MAP_LIST(TNX_GET_NAME, ComponentType, __VA_ARGS__)) \
    }; \
\
    FORCE_INLINE void Advance(uint32_t step) \
    { \
        __VA_OPT__(TNX_MAPF_LIST(TNX_BIND_ADVANCE, ComponentType, __VA_ARGS__)) \
    } \
\
    FORCE_INLINE void Bind(void** arrays, void* flagsArray, uint32_t startIndex = 0, int32_t count = -1) \
    { \
        int32_t arrayIndex = 0; \
        __VA_OPT__(TNX_MAPF_LIST(TNX_BIND_BIND, ComponentType, __VA_ARGS__)) \
    }

#define TNX_REGISTER_COMPONENT(ComponentType) \
    namespace { \
        struct _##ComponentType##_Registrar { \
            _##ComponentType##_Registrar() { \
                RegisterFieldsStatic<ComponentType<>>(); \
            } \
        }; \
        [[maybe_unused]] TNX_USED_ATTR static _##ComponentType##_Registrar _##ComponentType##_FieldsRegistered; \
    }

#define TNX_TEMPORAL_FIELDS(ComponentType, SysID, ...) \
    static inline CacheTier TemporalTier = CacheTier::Temporal; \
    static inline SystemID SystemTypeID = SystemID::SysID; \
    TNX_REGISTER_FIELDS(ComponentType, __VA_ARGS__)

#define TNX_VOLATILE_FIELDS(ComponentType, SysID, ...) \
    static inline CacheTier TemporalTier = CacheTier::Volatile; \
    static inline SystemID SystemTypeID = SystemID::SysID; \
    TNX_REGISTER_FIELDS(ComponentType, __VA_ARGS__)

// ---------------------------------------------------------------------------
// FlowState / GameMode self-registration macros
// ---------------------------------------------------------------------------

#define TNX_REGISTER_STATE(StateClass) \
    namespace { \
        struct _##StateClass##_StateReg { \
            _##StateClass##_StateReg() { \
                ReflectionRegistry::Get().RegisterState(#StateClass, \
                    []() -> std::unique_ptr<FlowState> { return std::make_unique<StateClass>(); }); \
            } \
        }; \
        [[maybe_unused]] TNX_USED_ATTR static _##StateClass##_StateReg _##StateClass##_state_reg; \
    }

#define TNX_REGISTER_MODE(ModeClass) \
    namespace { \
        struct _##ModeClass##_ModeReg { \
            _##ModeClass##_ModeReg() { \
                ReflectionRegistry::Get().RegisterMode(#ModeClass, \
                    []() -> std::unique_ptr<GameMode> { return std::make_unique<ModeClass>(); }); \
            } \
        }; \
        [[maybe_unused]] TNX_USED_ATTR static _##ModeClass##_ModeReg _##ModeClass##_mode_reg; \
    }

// ---------------------------------------------------------------------------
// TNX_REGISTER_MODEMIX — registers a user-defined GameMode mixin with the
// ReflectionRegistry. Assigns the next available ID from the user band
// (128–255) via g_GlobalMixinCounter, following the same pattern as
// component and entity registration.
//
// Usage (once per mixin type, in a .cpp file):
//   TNX_REGISTER_MODEMIX(MyAbilityMixin)
//
// Engine mixins (WithSpawnManagement, etc.) have fixed compile-time IDs in
// the engine band (0–127) and self-register by calling RegisterMixin directly.
// ---------------------------------------------------------------------------

#define TNX_REGISTER_MODEMIX(MixinClass) \
    namespace { \
        struct _##MixinClass##_MixinReg { \
            _##MixinClass##_MixinReg() { \
                uint8_t id = Internal::g_GlobalMixinCounter++; \
                assert(id <= ReflectionRegistry::MixinUserBandEnd && \
                    "TNX_REGISTER_MODEMIX: user mixin band exhausted (128-255)."); \
                ReflectionRegistry::Get().RegisterMixin(#MixinClass, id, /*isUserDefined=*/true); \
            } \
        }; \
        [[maybe_unused]] TNX_USED_ATTR static _##MixinClass##_MixinReg _##MixinClass##_mixin_reg; \
    }
