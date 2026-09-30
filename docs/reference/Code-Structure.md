# Code Structure — Headers Declare, Sources Define

> [Home](../Home.md) | [Design Decisions](Design-Decisions.md) | [Component System](../architecture/Component-System.md)

**Goal:** changing logic rebuilds the one `.cpp` that owns it — not every translation unit that includes a header.
Public headers describe *what* a system offers; `Private/*.cpp` files hold *how* it does it.

Link-time optimization is **off** (`INTERPROCEDURAL_OPTIMIZATION OFF`), so a function defined in a `.cpp` is never
inlined into other translation units. That is the cost to weigh against rebuild time, and it is why hot paths are an
explicit, documented exception rather than the default.

---

## The Four Cases

| Case | Example | Where the code lives |
|---|---|---|
| **1. Non-template code** | `PlayerInputLog`, `Logger`, `Archetype` | Declarations in the header; definitions in `Private/*.cpp` |
| **2. Templates with a closed set of instantiations** | `LogicThread<TNet, TRollback, TFrame>`, `World<…>` | Definitions in `.cpp`, explicit instantiation at the bottom of the `.cpp`, `extern template` in the header |
| **3. Templates over open / user types** | `Construct<T>`, `ConstructView<TEntity>` | Header, but thin: logic that doesn't depend on `T` moves into a non-template core in a `.cpp` |
| **4. Hot paths and compile-time code** | `FieldProxy`, SIMD traits, `Fixed32`, containers, schema validation | Header, by design |

### 1. Non-template code

Definitions go in `Private/*.cpp`. The only function bodies allowed in the header are:

- trivial accessors and mutators (a single statement),
- `constexpr` / `consteval` functions (they must be visible),
- case 4 hot paths, marked as such.

### 2. Closed-set templates — the `LogicThread` pattern

Engine policy templates are only ever instantiated with a known list of arguments. Treat them like non-template code:

```cpp
// LogicThread.h
template <typename TNet, typename TRollback, typename TFrame>
class LogicThread : public LogicThreadBase { void Start(); /* declarations only */ };

extern template class LogicThread<SoloSim, NoRollback, GameFrame>;
#ifdef TNX_ENABLE_NETWORK
extern template class LogicThread<AuthoritySim, NoRollback, GameFrame>;
#endif
// ...

// LogicThread.cpp
#define TMPL template <typename TNet, typename TRollback, typename TFrame>
TMPL void LogicThread<TNet, TRollback, TFrame>::Start() { ... }

template class LogicThread<SoloSim, NoRollback, GameFrame>;
#ifdef TNX_ENABLE_NETWORK
template class LogicThread<AuthoritySim, NoRollback, GameFrame>;
#endif
```

- The `extern template` list in the header and the explicit instantiation list in the `.cpp` must use the **same
  feature guards** (`TNX_ENABLE_NETWORK`, `TNX_ENABLE_ROLLBACK`, …).
- Canonical examples: `LogicThread.h` / `LogicThread.cpp`, `World.h` / `World.cpp`,
  `TemporalComponentCache.h` / `.cpp` (`ComponentCache<Tier>`), `NetThreadBase.h` / `.cpp`.
- If a closed-set template's member templates are only instantiated from one `.cpp`, their bodies can live in a
  private header included only there — e.g. `Core/Private/RollbackImpl.h`, included only by `LogicThread.cpp`.
- When the header can't name the instantiation arguments (they're declared later), omit `extern template`: with the
  bodies out of the header, other translation units can't instantiate implicitly and link against the explicit
  instantiation (`NetThreadBase<AuthorityNet>` etc.).
- Member function templates on a closed parameter (e.g. `template <typename TLogic>` where `TLogic` is always a
  `LogicThread` specialization) follow the same rule with explicit member instantiations.

### 3. Open templates — thin header, non-template core

Templates instantiated with user types (Constructs, entity types, component views) must stay in headers. Keep them
thin: anything that doesn't depend on `T` moves into a non-template function in a `.cpp`, and the template becomes a
small adapter.

Canonical examples:

- `Construct<Derived>` derives from the non-template `ConstructBase` (`Construct.cpp`), which owns world/soul/ID state,
  the View list, contact unbinding, and tick deregistration. The template keeps only concept-detected wiring.
- `ReplicationSystem::RegisterConstruct<T>` collects the view handles (the only `T`-dependent call) and forwards to
  `RegisterConstructCore`.
- `ConstructRegistry::Create<T>` builds the type-erased entry with `MakeEntry<T>` and files it with the non-template
  `AddEntry`.
- `AuthoritySim::OnSimInput<TLogic>` calls the non-template `AuthoritySim::RunSimInput` in `AuthorityNet.cpp`.

### 4. Hot paths and compile-time code

Some code must be visible to the optimizer at every call site, or is evaluated at compile time. These headers are
intentionally implementation-heavy:

| Header | Reason |
|---|---|
| `FieldProxy.h`, `SIMDTraits.h`, `VecMath.h`, `FieldMath.h`, `QuatMath.h` | Inner loops of 8-wide SIMD sweeps |
| `SimFloat.h`, `Fixed32.h`, `FixedMath.h`, `FixedTrig.h`, `FastTrig.h` | Scalar math used in every sweep |
| `FlatMap.h`, `FixedBitset.h`, `PagedMap.h`, `Trinyx*Ring*.h` | Generic containers |
| `Events.h` | Generic delegates (`Callback`, `MultiCallback`) over arbitrary signatures |
| `SchemaValidation.h`, `SchemaReflector.h`, `Types.h` | Compile-time validation and core types |

Hot-path functions outside these headers need a short justification at the definition:

```cpp
// inline: hot path — called per entity in the PrePhysics sweep
FORCE_INLINE void Foo() { ... }
```

---

## Include Hygiene

- **Forward-declare** in headers whenever a pointer or reference is enough; include in the `.cpp`.
- **Keep heavy third-party headers out of public headers** (Jolt, Vulkan, SDL, GameNetworkingSockets, ImGui) unless
  their types are part of the API. Use opaque handles or a private implementation struct.
- **`.inl` files are not separation.** Every includer still compiles them. Use them only to organize template
  definitions that must be visible (case 3 or 4).
- **Registration macros** generate as little code as possible and call out-of-line helpers.

---

## Byte-Copied Struct Layouts

Structs that are `memcpy`'d across a boundary (network wire, disk files, GPU buffers, rollback records) state the size
the engine expects, as one literal next to the struct:

```cpp
static_assert(sizeof(NetInputFrame) == 156, "NetInputFrame must be 156 bytes");
```

The number is the contract: packet budgets, file formats and shader mirrors are designed around it, so any change to it
is deliberate and visible in review. Intended padding is an explicit `_Pad` member.

- **One number, not a sum:** never restate the members as `4 + 76 + 4 + 64`. The sum has to be edited with every
  field and still says nothing about what size was intended.
- **Buffer bounds derive from types:** size scratch buffers from `sizeof` of the structs they hold, not from restated
  field widths.
- **Alignment padding is the compiler's job:** `alignas(64)` already rounds `sizeof` up to whole cache lines; don't
  add a hand-computed tail array.
- **Variable wire formats have one codec:** the writer and reader share a single table of what goes on the wire
  (`InputWindowCodec` is the pattern), so adding a field is one row and neither side counts bytes.

---

## Review Checklist

- Does this header change force a rebuild that a `.cpp` change would not have?
- Is every function body in the header trivial, `constexpr`, a closed-set template that should be in a `.cpp`, an open
  template that's as thin as it can be, or a justified hot path?
- Are `extern template` and explicit instantiation lists in sync, with matching feature guards?
- Could an include in this header be a forward declaration?
- Does every byte-copied struct assert its expected size as one literal, and is every buffer bound derived from `sizeof`?

`clang-tidy`'s `misc-definitions-in-headers` catches non-`inline` definitions in headers; the rest is review.
