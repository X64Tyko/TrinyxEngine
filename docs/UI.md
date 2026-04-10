# UI Framework (TrinyxEngine)

> **Navigation:** [← Architecture](ARCHITECTURE.md) | [← Status](STATUS.md) | [← Back to README](../README.md)

---

## 1. Overview

Trinyx has two distinct UI surfaces:

1. **Editor UI** — developer tooling: scene hierarchy, entity inspection, property grids, content browser, PIE controls, and the upcoming visual scripting canvas.
2. **Runtime HUD** — in-game overlay: health bars, ammo counts, crosshairs, minimaps, scoreboards, and any designer-authored screen-space elements.

The two surfaces have very different requirements. The decision for each is driven by Trinyx's goals: fast iteration, determinism, ECS-native data access, and keeping the engine lean during the R&D phase.

---

## 2. Editor UI — Keep Dear ImGui

### Decision

**Verdict: Keep Dear ImGui. Do not roll a custom editor UI.**

The editor already uses Dear ImGui (docking branch) wired through `EditorRenderer` → `EditorContext` → panel hierarchy (`DetailsPanel`, `WorldOutlinerPanel`, `ContentBrowserPanel`, `LogPanel`, `EngineStatsPanel`). This is working, it is the right tool, and the cost of replacing it is not justified.

### Why ImGui Wins for the Editor

| Requirement | ImGui delivers |
|---|---|
| Docking / multi-panel layout | ✅ Built-in via `ImGuiConfigFlags_DockingEnable` |
| Property grid / reflected fields | ✅ Implemented — `DetailsPanel` drives off field metadata |
| Node editor (visual scripting canvas) | ✅ `imnodes` / `imgui-node-editor` drop in cleanly |
| Hot reload of editor panels | ✅ Immediate-mode — no retained tree to invalidate |
| PIE viewport texture display | ✅ `ImGui::Image()` + `VkDescriptorSet` already implemented |
| GPU picking | ✅ Integrated (scatter_pick + sort_instances_pick shaders) |
| Cross-platform | ✅ SDL3 + Vulkan backends already wired |
| Maintenance cost | ✅ Near-zero — mature, actively maintained, header-only |

### When Would a Custom Editor UI Make Sense?

A fully custom editor UI (Qt, a proprietary retained-mode system, or something bespoke) would only be worth the investment if:

- Trinyx needed a **polished, consumer-facing editor** shipped as a product (not an R&D tool).
- High-DPI theming, animated transitions, or complex responsive layout were first-class requirements.
- The team had dedicated UI engineering bandwidth for multi-year maintenance.

None of those apply. The editor is a developer tool. ImGui is the correct tool for developer tools.

### Visual Scripting Canvas

The visual scripting system (Blueprint-style codegen) fits naturally into ImGui's immediate-mode model. Recommended integration path:

- [`imgui-node-editor`](https://github.com/thedmd/imgui-node-editor) — full-featured, production-proven, Bezier links, zooming, selection.
- [`imnodes`](https://github.com/Nelarius/imnodes) — simpler alternative if a lighter graph is sufficient.

Both slot into the existing `EditorPanel` → `EditorContext::BuildFrame()` call path with no structural changes to the engine.

---

## 3. Runtime HUD — HudContext Pattern

### Decision

**Verdict: Use Dear ImGui for the HUD during the R&D phase via a swappable `HudContext` interface. Do not roll a custom HUD renderer yet.**

### Why Not Roll a Custom HUD Renderer Now?

Rolling a custom retained-mode HUD (layout engine, style sheets, animation, text shaping) is a multi-year project. It provides no architectural advantage over ImGui in the R&D phase, and the Trinyx architecture already makes swapping it out clean when the time comes (see §3.3 below).

### Why Not Use ImGui Directly in Game Code?

Calling `ImGui::*` directly from `GameMode`, `Construct` ticks, or ECS systems would couple game code tightly to the ImGui API and scatter HUD calls across unrelated systems. Instead, game code should push data to a `HudContext`; the `HudContext` owns the rendering.

### 3.1 HudContext Interface

`HudContext` is a pure virtual base. The active implementation is set by game code at startup; the `GameplayRenderer` owns a pointer and calls it from its `RecordOverlay` hook.

```cpp
// src/Runtime/Rendering/Public/HudContext.h
class HudContext
{
public:
    virtual ~HudContext() = default;

    /// Called once per render frame, between ImGui::NewFrame() and ImGui::Render()
    /// (or the equivalent for non-ImGui backends). Push all HUD elements here.
    virtual void BuildFrame() = 0;
};
```

A null implementation (`NullHudContext`) is the default — zero overhead for builds that don't need a HUD.

### 3.2 ECS Integration Pattern

The HUD reads entity data through the same SoA FieldProxy cursors available everywhere else. It does **not** subscribe to events, hold entity handles between frames, or cache pointers across frame boundaries. The pattern is a simple frame-scoped read:

```cpp
class ArenaHud : public HudContext
{
    World* GameWorld = nullptr;   // set at initialization

    void BuildFrame() override
    {
        // Read directly from ECS — no coupling to specific entities.
        // Game code registers "which entity is the local player" once at spawn.
        if (!LocalPlayerView.IsValid()) return;

        float hp = LocalPlayerView.Health.Current;

        ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##hud", nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoInputs);
        ImGui::ProgressBar(hp / MaxHp, {200, 16}, "");
        ImGui::End();
    }
};
```

Key constraints:
- `BuildFrame()` runs on the **Encoder (render) thread**. Read from the read-side SoA slab only. Do not write entity data from HudContext.
- HUD elements read the **most recently published frame** via the existing triple-buffer mechanism — the same data the scatter shader reads. No special synchronization needed.
- `HudContext` does not tick on the Brain thread and does not participate in rollback. HUDs display interpolated presentation state, not simulation state.

### 3.3 Swap Path (Future)

When the HUD outgrows ImGui — e.g., for motion graphics, animated ability indicators, or complex layout — the swap is contained:

1. Implement a new `class CustomHud : public HudContext`.
2. Replace the `GameplayRenderer::HudLayer` pointer at startup.
3. Delete `ImGuiHudContext`.

No game code changes. No engine changes. The interface is the contract.

Candidates when that time comes:

| Option | Fit | Notes |
|---|---|---|
| **RmlUI** | Good | CSS-like layout, HTML data model, Vulkan backend available. Retained-mode but data-driven. |
| **Custom minimal** | Good if tiny | If HUD needs only a handful of element types (bars, text, icons), a bespoke Vulkan draw call layer is ~500 LOC and zero overhead. |
| **Keep ImGui** | Fine | ImGui's `ImDrawList` API can produce polished HUDs. Many shipped games use it. |

---

## 4. Threading Boundaries

| UI Surface | Thread | Notes |
|---|---|---|
| Editor panels (`EditorContext::BuildFrame`) | Encoder | Between `ImGui::NewFrame()` and `ImGui::Render()` |
| Runtime HUD (`HudContext::BuildFrame`) | Encoder | Same slot — HUD and editor are mutually exclusive (one is active at a time) |
| ImGui event push (`EditorRenderer::PushImGuiEvent`) | Sentinel | Thread-safe via `ImGuiEventQueue` |
| ECS reads inside HudContext | Encoder | Read from published read-side slab only |

ImGui's immediate-mode model is single-threaded within a frame. The Encoder thread owns the ImGui frame from `NewFrame()` to `Render()`. No other thread should call ImGui APIs.

---

## 5. Build Configuration

| Build | Editor UI | Runtime HUD |
|---|---|---|
| `TNX_ENABLE_EDITOR=ON` | ImGui + docking | `HudContext` active (game code provides implementation) |
| `TNX_ENABLE_EDITOR=OFF` | None | `HudContext` active (game code provides implementation) |

When `TNX_ENABLE_EDITOR=OFF`, Dear ImGui is compiled out of the TrinyxEngine library target entirely. The `HudContext` implementation provided by the game (`Testbed` or shipped game) is responsible for including and initializing its own renderer. During the R&D phase, `Testbed` ships with `TNX_ENABLE_EDITOR=ON` and an `ImGuiHudContext` that reuses the already-initialized ImGui context.

---

## 6. What Trinyx's ECS Architecture Gives the HUD for Free

Trinyx's SoA layout and dirty-bit tracking benefit HUD rendering directly:

- **No per-entity query.** HUD reads field arrays directly — `Transform.PosX[entityIndex]` — without map lookups or virtual dispatch.
- **Temporal interpolation already done.** The scatter shader writes interpolated world-space positions into `InstanceBuffer`. If a HUD needs world-to-screen projection of an entity (e.g., a damage number floating above a character), it reads the already-interpolated GPU buffer, not raw simulation state.
- **Rollback-transparent.** HUD reads presentation state (post-interpolation), not simulation state. A rollback that corrects frame N does not stutter the HUD because the HUD was never reading frame N directly.

---

## 7. Out of Scope

These are deliberately excluded from the HUD system to keep it lean:

- **Animation system.** Animated HUD transitions (fade, slide, pulse) are the game's responsibility — either hand-coded in `HudContext::BuildFrame()` using ImGui's animation utilities or deferred to a future HUD framework.
- **Data binding / reactive UI.** No MVVM, no signals/slots, no property observers. The immediate-mode push model (`BuildFrame()` reads what it needs each frame) is intentional.
- **Serialization.** HUD layout is code, not data. If designer-editable HUD layout is needed, consider a future JSON-driven layout layer on top of HudContext.
