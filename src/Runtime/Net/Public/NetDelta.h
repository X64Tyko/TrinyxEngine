#pragma once
#include "FieldProxy.h"
#include "Types.h"
#include <concepts>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <unordered_map>
#include <utility>

// ---------------------------------------------------------------------------
// NetDelta — per-component delta serialization infrastructure.
//
// Each temporal component may opt into custom serialization by defining:
//   static void DeltaSerialize(const ComponentDeltaCtx&, NetDeltaWriter&);
//   static bool DeltaDeserialize(const ComponentDeltaCtx&, NetDeltaReader&);
//
// Components that do not define these get a generated default that emits a
// field-presence bitmask followed by only the changed field values. The
// default compares current vs. baseline field-by-field using exact equality;
// custom implementations may use tolerances, compressed encodings, etc.
//
// Call site (future ServerClientChannel):
//   1. Build the current-frame and baseline-frame field array tables for the
//      archetype (via Archetype::BuildFieldArrayTable at each frame number).
//   2. Slice each component's field range out of both tables.
//   3. Fill ComponentDeltaCtx and call InvokeDeltaSerialize<TComp>.
//   4. For type-erased runtime dispatch, use MakeComponentDeltaFns<TComp>()
//      and store the result in the component registry.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// NetDeltaWriter — lightweight streaming byte writer over a caller-supplied
// buffer. Writes are little-endian (engine is x86/x64 only). On overflow the
// Overflow flag is set and all subsequent writes become no-ops; the caller
// checks IsOk() after the component payload is complete.
// ---------------------------------------------------------------------------
struct NetDeltaWriter
{
	uint8_t* Buf  = nullptr;
	uint32_t Pos  = 0;
	uint32_t Cap  = 0;
	bool Overflow = false;

	void WriteU8(uint8_t v)
	{
		if (Pos + 1 > Cap)
		{
			Overflow = true;
			return;
		}
		Buf[Pos++] = v;
	}

	void WriteU16(uint16_t v)
	{
		if (Pos + 2 > Cap)
		{
			Overflow = true;
			return;
		}
		std::memcpy(Buf + Pos, &v, 2);
		Pos += 2;
	}

	void WriteU32(uint32_t v)
	{
		if (Pos + 4 > Cap)
		{
			Overflow = true;
			return;
		}
		std::memcpy(Buf + Pos, &v, 4);
		Pos += 4;
	}

	void WriteBytes(const void* src, uint32_t len)
	{
		if (Pos + len > Cap)
		{
			Overflow = true;
			return;
		}
		std::memcpy(Buf + Pos, src, len);
		Pos += len;
	}

	uint32_t BytesWritten() const { return Pos; }
	bool IsOk() const { return !Overflow; }
};

// ---------------------------------------------------------------------------
// NetDeltaReader — matching streaming reader. Error flag is sticky; partial
// reads after error return zero-initialised values and the caller checks
// IsOk() once after the component payload is consumed.
// ---------------------------------------------------------------------------
struct NetDeltaReader
{
	const uint8_t* Buf = nullptr;
	uint32_t Pos       = 0;
	uint32_t Size      = 0;
	bool Error         = false;

	uint8_t ReadU8()
	{
		if (Pos + 1 > Size)
		{
			Error = true;
			return 0;
		}
		return Buf[Pos++];
	}

	uint16_t ReadU16()
	{
		if (Pos + 2 > Size)
		{
			Error = true;
			return 0;
		}
		uint16_t v;
		std::memcpy(&v, Buf + Pos, 2);
		Pos += 2;
		return v;
	}

	uint32_t ReadU32()
	{
		if (Pos + 4 > Size)
		{
			Error = true;
			return 0;
		}
		uint32_t v;
		std::memcpy(&v, Buf + Pos, 4);
		Pos += 4;
		return v;
	}

	void ReadBytes(void* dst, uint32_t len)
	{
		if (Pos + len > Size)
		{
			Error = true;
			return;
		}
		std::memcpy(dst, Buf + Pos, len);
		Pos += len;
	}

	bool IsOk() const { return !Error; }
};

// ---------------------------------------------------------------------------
// ComponentDeltaCtx — passed to DeltaSerialize / DeltaDeserialize.
//
// Current[0..N-1]  — per-field array pointers for this component at the
//                    frame being serialized (write frame on Authority, receive
//                    frame on Owner).
// Baseline[0..N-1] — per-field array pointers at the last acknowledged frame
//                    for this client. Unused by DeltaDeserialize; may be null.
// Index            — entity slot index into each field array.
//
// Arrays are indexed 0..N-1 matching the order returned by DefineFields().
// ---------------------------------------------------------------------------
struct ComponentDeltaCtx
{
	void* const* Current  = nullptr;
	void* const* Baseline = nullptr;
	uint32_t Index        = 0;
};

// ---------------------------------------------------------------------------
// FieldProxyExtract — extracts ValueType from a FieldProxy member pointer.
// Matches FieldProxy<FT, W> Class::* and yields FT.
// ---------------------------------------------------------------------------
template <typename T>
struct FieldProxyExtract;

template <typename FT, FieldWidth W, typename Class>
struct FieldProxyExtract<FieldProxy<FT, W> Class::*>
{
	using ValueType = FT;
};

// ---------------------------------------------------------------------------
// Concepts — detect whether a component provides custom implementations.
// ---------------------------------------------------------------------------
template <typename T>
concept HasCustomDeltaSerialize =
	requires(const ComponentDeltaCtx& ctx, NetDeltaWriter& w) {
		{ T::DeltaSerialize(ctx, w) } -> std::same_as<void>;
	};

template <typename T>
concept HasCustomDeltaDeserialize =
	requires(const ComponentDeltaCtx& ctx, NetDeltaReader& r) {
		{ T::DeltaDeserialize(ctx, r) } -> std::same_as<bool>;
	};

// ---------------------------------------------------------------------------
// DeltaDetail — per-field helpers.
//
// Standalone function templates rather than nested lambdas: MSVC 2022 has
// known issues resolving `using` type aliases inside immediately-invoked
// lambdas that are fold-expanded inside an outer explicit-template lambda.
// Flat function templates with the field index as a non-type parameter sidestep
// the problem entirely.
// ---------------------------------------------------------------------------
namespace DeltaDetail
{
template <size_t I, typename TComp>
bool FieldChanged(const ComponentDeltaCtx& ctx)
{
	using Fields       = decltype(TComp::DefineFields());
	using ValueT       = typename FieldProxyExtract<std::tuple_element_t<I, Fields>>::ValueType;
	const ValueT* cur  = static_cast<const ValueT*>(ctx.Current[I]) + ctx.Index;
	const ValueT* base = static_cast<const ValueT*>(ctx.Baseline[I]) + ctx.Index;
	return *cur != *base;
}

template <size_t I, typename TComp>
void WriteField(const ComponentDeltaCtx& ctx, NetDeltaWriter& writer)
{
	using Fields      = decltype(TComp::DefineFields());
	using ValueT      = typename FieldProxyExtract<std::tuple_element_t<I, Fields>>::ValueType;
	const ValueT* cur = static_cast<const ValueT*>(ctx.Current[I]) + ctx.Index;
	writer.WriteBytes(cur, sizeof(ValueT));
}

template <size_t I, typename TComp>
void ReadField(const ComponentDeltaCtx& ctx, NetDeltaReader& reader)
{
	using Fields = decltype(TComp::DefineFields());
	using ValueT = typename FieldProxyExtract<std::tuple_element_t<I, Fields>>::ValueType;
	ValueT* dst  = static_cast<ValueT*>(ctx.Current[I]) + ctx.Index;
	reader.ReadBytes(dst, sizeof(ValueT));
}
} // namespace DeltaDetail

// ---------------------------------------------------------------------------
// DefaultDeltaSerialize<TComp>
//
// Iterates DefineFields() at compile time. For each field, compares
// Current[i][Index] against Baseline[i][Index] using exact equality and sets
// the corresponding bit in a field-presence mask. Then writes:
//   [mask: uint8/16/32 depending on field count] [changed field values...]
//
// TComp must be the scalar instantiation (e.g. CTransform<>).
// ---------------------------------------------------------------------------
template <typename TComp>
void DefaultDeltaSerialize(const ComponentDeltaCtx& ctx, NetDeltaWriter& writer)
{
	constexpr size_t N = std::tuple_size_v<decltype(TComp::DefineFields())>;
	static_assert(N <= 32, "Component exceeds 32 fields — widen mask type in DefaultDeltaSerialize");

	uint32_t mask = 0;
	[&]<size_t... Is>(std::index_sequence<Is...>)
	{
		((DeltaDetail::FieldChanged<Is, TComp>(ctx) ? (void)(mask |= (1u << Is)) : (void)0), ...);
	}(std::make_index_sequence<N>{});

	if constexpr (N <= 8)
		writer.WriteU8(static_cast<uint8_t>(mask));
	else if constexpr (N <= 16)
		writer.WriteU16(static_cast<uint16_t>(mask));
	else
		writer.WriteU32(mask);

	[&]<size_t... Is>(std::index_sequence<Is...>)
	{
		((mask & (1u << Is) ? DeltaDetail::WriteField<Is, TComp>(ctx, writer) : (void)0), ...);
	}(std::make_index_sequence<N>{});
}

// ---------------------------------------------------------------------------
// DefaultDeltaDeserialize<TComp>
//
// Reads the mask written by DefaultDeltaSerialize and applies only the fields
// flagged as changed into Current[i][Index]. Baseline is not used.
// Returns false if the reader hit an error (truncated packet).
// ---------------------------------------------------------------------------
template <typename TComp>
bool DefaultDeltaDeserialize(const ComponentDeltaCtx& ctx, NetDeltaReader& reader)
{
	constexpr size_t N = std::tuple_size_v<decltype(TComp::DefineFields())>;

	uint32_t mask = 0;
	if constexpr (N <= 8)
		mask = reader.ReadU8();
	else if constexpr (N <= 16)
		mask = reader.ReadU16();
	else
		mask = reader.ReadU32();

	[&]<size_t... Is>(std::index_sequence<Is...>)
	{
		((mask & (1u << Is) ? DeltaDetail::ReadField<Is, TComp>(ctx, reader) : (void)0), ...);
	}(std::make_index_sequence<N>{});

	return reader.IsOk();
}

// ---------------------------------------------------------------------------
// InvokeDeltaSerialize / InvokeDeltaDeserialize
//
// Concept-conditional dispatch: use the component's own implementation if
// present, otherwise fall back to the field-by-field default.
// ---------------------------------------------------------------------------
template <typename TComp>
void InvokeDeltaSerialize(const ComponentDeltaCtx& ctx, NetDeltaWriter& writer)
{
	if constexpr (HasCustomDeltaSerialize<TComp>)
		TComp::DeltaSerialize(ctx, writer);
	else
		DefaultDeltaSerialize<TComp>(ctx, writer);
}

template <typename TComp>
bool InvokeDeltaDeserialize(const ComponentDeltaCtx& ctx, NetDeltaReader& reader)
{
	if constexpr (HasCustomDeltaDeserialize<TComp>)
		return TComp::DeltaDeserialize(ctx, reader);
	else
		return DefaultDeltaDeserialize<TComp>(ctx, reader);
}

// ---------------------------------------------------------------------------
// ComponentDeltaFns — type-erased function pointer pair for runtime dispatch.
//
// The template-based InvokeDelta* functions can't be called from the archetype
// iterator which operates on runtime type IDs. Store these in the component
// registry (keyed by ComponentTypeID) so ServerClientChannel can call them
// without knowing TComp at the call site.
//
// Usage:
//   ComponentDeltaFns fns = MakeComponentDeltaFns<CTransform<>>();
//   fns.Serialize(ctx, writer);
//   fns.Deserialize(ctx, reader);
// ---------------------------------------------------------------------------
struct ComponentDeltaFns
{
	void (*Serialize)(const ComponentDeltaCtx& ctx, NetDeltaWriter& writer)   = nullptr;
	bool (*Deserialize)(const ComponentDeltaCtx& ctx, NetDeltaReader& reader) = nullptr;
	CacheSlotID CacheSlot                                                     = 0; // StaticTemporalIndex() — used by DispatchDeltaCorrectionJobs

	bool IsValid() const { return Serialize != nullptr; }
};

template <typename TComp>
ComponentDeltaFns MakeComponentDeltaFns()
{
	return {
		[](const ComponentDeltaCtx& ctx, NetDeltaWriter& writer)
	{
		InvokeDeltaSerialize<TComp>(ctx, writer);
	},
		[](const ComponentDeltaCtx& ctx, NetDeltaReader& reader) -> bool
	{
		return InvokeDeltaDeserialize<TComp>(ctx, reader);
	}
	};
}

// ---------------------------------------------------------------------------
// ComponentDeltaRegistry — maps ComponentTypeID to its delta function pair.
//
// Populated at static-init time by TNX_NET_TEMPORAL; looked up at runtime by
// ServerClientChannel when iterating dirty entities.
// ---------------------------------------------------------------------------
class ComponentDeltaRegistry
{
public:
	static ComponentDeltaRegistry& Get()
	{
		static ComponentDeltaRegistry Instance;
		return Instance;
	}

	void Register(ComponentTypeID typeID, ComponentDeltaFns fns)
	{
		Table[typeID] = fns;
	}

	// Returns nullptr if the component was not registered for delta serialization.
	const ComponentDeltaFns* Find(ComponentTypeID typeID) const
	{
		auto it = Table.find(typeID);
		return it != Table.end() ? &it->second : nullptr;
	}

	// Iterate all registered entries. Fn signature: void(ComponentTypeID, const ComponentDeltaFns&).
	template <typename Fn>
	void ForEach(Fn&& fn) const
	{
		for (const auto& [id, fns] : Table)
			fn(id, fns);
	}

private:
	std::unordered_map<ComponentTypeID, ComponentDeltaFns> Table;
};

// ---------------------------------------------------------------------------
// TNX_NET_TEMPORAL(ComponentType) — place in a temporal component's header
// alongside TNX_REGISTER_COMPONENT to register delta serialization fns.
//
// The default generated by MakeComponentDeltaFns does field-by-field exact
// comparison. Components with custom DeltaSerialize / DeltaDeserialize statics
// are picked up automatically via concept detection in InvokeDeltaSerialize.
//
// Example (CTransform.h):
//   TNX_REGISTER_COMPONENT(CTransform)
//   TNX_NET_TEMPORAL(CTransform)
// ---------------------------------------------------------------------------
#define TNX_NET_TEMPORAL(ComponentType)                                                     \
	namespace                                                                               \
	{                                                                                       \
	struct _##ComponentType##_DeltaRegistrar                                                \
	{                                                                                       \
		_##ComponentType##_DeltaRegistrar()                                                 \
		{                                                                                   \
			ComponentDeltaFns fns = MakeComponentDeltaFns<ComponentType<>>();               \
			fns.CacheSlot         = ComponentType<>::StaticTemporalIndex();                 \
			ComponentDeltaRegistry::Get().Register(                                         \
				ComponentType<>::StaticTypeID(), fns);                                      \
		}                                                                                   \
	};                                                                                      \
	[[maybe_unused]] static _##ComponentType##_DeltaRegistrar _##ComponentType##_delta_reg; \
	}
