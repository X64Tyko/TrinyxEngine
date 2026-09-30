# Editor Overview

> [Home](../Home.md) | [Debugging →](Debugging.md)

---

## Overview

The editor is enabled by `-DTNX_ENABLE_EDITOR=ON`. It runs on the Encoder (render) thread and operates directly on live ECS slab data — not a copy. Scope is intentionally limited to scene editing, entity inspection, and PIE testing.

**Panels (8):** World Outliner, Details, Content Browser, Engine Stats, Log, Node Script, Component Generator, Debugger

**Editor windows (3):** Construct Editor, Entity Editor, Prefab Editor — authoring windows for Constructs, entity types,
and prefabs. The Construct Editor uses `TrinyxParser` to turn annotated Construct C++ methods into node graphs.

**Windowing:** everything lives in one main window. ImGui multi-viewport support is disabled (`f095d3b`) — see
[Known Issues](../reference/Known-Issues.md) #14.

---

## Core Components

### `EditorContext`

Main UI manager. Owns all panels. Called once per render frame between `ImGui::NewFrame()` and `ImGui::Render()`.

- Panel registration via `AddPanel<T>(...)`
- Scene load/save (`LoadScene`, `SaveToFile`)
- Entity spawn/delete via the engine spawn handshake
- Prefab instantiation (`SpawnPrefab`)
- Undo/redo stack (max 50 commands, merge-capable)
- PIE lifecycle (`StartPIE`, `StopPIE`)
- Scene snapshot/restore for play/stop cycles
- Gizmo rendering (`DrawGizmo` — ImGuizmo)
- File drag-drop handling

### `EditorState`

Transient per-frame state shared across all panels. Lives on the render thread — no synchronization required.

| Field | Description |
|---|---|
| `SelectionType` | `None` / `Archetype` / `Entity` |
| Selected entity triple-index | ClassID, Archetype*, Chunk*, LocalIndex, CacheIndex |
| `GizmoOp` | Translate / Rotate / Scale |
| `bGizmoWorldMode` | World vs local space |
| `GizmoSnapTranslate/Rotate/Scale` | Snap increments (default 0.5 / 15° / 0.25) |
| `CurrentScenePath` / `CurrentSceneName` | Loaded .tnxscene |
| `bSceneDirty` | Unsaved-changes flag (shown as `*` in menu bar) |
| `SceneDefaultState` / `SceneDefaultMode` | FlowState and GameMode names for PIE |
| Engine references | Registry, EngineConfig, LogicThread, TrinyxEngine, MeshManager, AssetDatabase |
| `ReplicationSystem*` | Non-null only during networked PIE |

---

## Panels

### World Outliner

Hierarchical entity browser. Archetypes as collapsible tree nodes showing entity count. Chunks within each archetype; individual entities as leaves (cache index). Right-click → Delete. Delete key removes selected entity when panel is focused. Accepts prefab drag-drops from Content Browser.

### Details

Component field inspector. In archetype mode: shows DebugName, ClassID, entity/chunk counts, component list. In entity mode: shows editable field values (Float32, Fixed32, Float64, Int32, Uint32, asset references). Asset reference fields show as combo dropdowns. Editing is disabled while simulation is running. All edits mark entity dirty, push an undo command, and set `bSceneDirty`.

### Content Browser

Asset manager for the project content directory. Table view: Path, Type, MeshID, UUID. Type filter combo. Import Mesh button. Drag prefab → World Outliner to instantiate. Drag mesh → Details asset field. Double-click level files to load the scene.

### Engine Stats

Real-time telemetry: Render FPS/ms, Logic FPS/ms, fixed-step FPS/ms, current logic frame number, total entity and chunk counts. Full `EngineConfig` dump: all timing, budget, and physics config fields.

### Log

Real-time log viewer with per-level filter checkboxes (Trace / Debug / Info / Warn / Error / Fatal). Color-coded output. Auto-scroll toggle. Reads from `Logger::Get().GetLogRing()` ring buffer.

### Node Script

Visual blueprint-style scripting with C++ code generation.

**25 node types across 5 categories:**

| Category | Nodes |
|---|---|
| Events | OnPrePhysics, OnPostPhysics, OnUpdate, OnSpawn, OnDestroy |
| Flow | Sequence (2-way), Branch (conditional exec split) |
| Properties | GetProperty, SetProperty |
| Math | Add, Subtract, Multiply, Divide, Clamp, Lerp |
| Vectors | Make Vec3, Break Vec3, Length, Normalize, Scale |
| Entity | GetPosition, SetPosition, GetVelocity, SetVelocity, ApplyImpulse |

Code generation: topological walk from event roots; validates determinism constraints (Branch nodes rejected in pre/post-physics paths); outputs C++ to file.

### Component Generator

Form-based ECS component header generator. Name + storage tier (Temporal / Volatile / Cold) + SystemGroup + dynamic field list (Name + Type). Live code preview. Emits the correct macro (`TNX_TEMPORAL_FIELDS` / `TNX_VOLATILE_FIELDS` / `TNX_REGISTER_FIELDS`).

### Debugger

PIE and networking diagnostics.

**Network tab:** Active channel count, dirty entity count, StateCorrection bytes/frame, EntityDelta bytes/frame, delta vs full-state ratio. 128-sample ring buffer plots.

**Profiler tab:** Fixed update budget (1000/Hz ms) with color-coded status (green / yellow at 80% / red at over). Logic thread and fixed-step frame time history plots.

---

## Asset Database

UUID↔path authority. Populated at startup; used by the runtime `AssetRegistry`.

**Sidecar files (.tnxid):** Binary, 8 bytes UUID + 8 bytes ContentHash + 4 bytes SchemaVersion + 1 byte Flags. Created once per asset at import.

`SidecarFlags`: `Dirty` (content hash mismatch), `SchemaOutdated` (importer version changed).

**Reconciliation:** Runs at `Initialize()` and on-demand. Scans content directory, creates .tnxid for new assets, marks Dirty on hash mismatch, detects moved assets (UUID stable, path updated).

**Persistence:** `.tnxdb` file in content root.

---

## Scene File Format (.tnxscene)

JSON:

```json
{
  "name": "TestArena",
  "defaultState": "GameplayState",
  "defaultMode": "ArenaMode",
  "entities": [ ... ]
}
```

Per-entity fields serialized as named key-value pairs. Prefab files are single-entity `.tnxscene` files.

**Load:** JSON → `EntityBuilder::SpawnScene()` via spawn handshake → update `EditorState` metadata.

**Save:** `SaveToFile()` serializes all registry entities.

---

## Play-In-Editor (PIE)

**Local play:**
1. `SnapshotScene()` captures all archetype field data
2. Simulation unpauses; input routes to the active world
3. `StopPIE()` → `RestoreSnapshot()` byte-restores all field data

**Networked PIE (1–4 clients):**
- Each client gets its own `WorldViewport`, `FlowManager`, and engine config instance
- Server may be headless (no viewport) or rendered
- `ReplicationSystem` is activated on the server world
- PIE uses real GNS loopback — bugs found in PIE are real bugs

---

## Gizmo (ImGuizmo)

Reads PosXYZ, RotQxyzw, ScaleXYZ from entity field arrays via reflection. Decomposes manipulated matrix back to component fields.

- **W** — Translate, **E** — Rotate, **R** — Scale
- World vs local space toggle
- Snap: 0.5 units / 15° / 0.25 scale (configurable)
- Every manipulation pushes an `EntityTransformCommand`

---

## Undo / Redo

Stack-based, max 50 commands. Adjacent compatible commands merge.

| Command | Merges? |
|---|---|
| `EntityTransformCommand` | Yes — same entity (JSON field snapshot) |
| `ComponentFieldChangeCommand` | Yes — same field (old/new bytes) |

Ctrl+Z / Ctrl+Y.

---

## Keyboard Shortcuts

| Key | Action |
|---|---|
| W / E / R | Gizmo: Translate / Rotate / Scale |
| Ctrl+Z / Ctrl+Y | Undo / Redo |
| Ctrl+O / Ctrl+S / Ctrl+Shift+S | Open / Save / Save As |
| Delete | Delete selected entity |
| Escape | Stop PIE |
| Shift+F1 | Toggle mouse capture |

---

## GPU Picking

Enabled automatically with `TNX_ENABLE_EDITOR=ON`. Each viewport owns a `R32_UINT` pick target — stores entity cache index per pixel. Click-to-select resolves the CacheIndex, then walks the registry to find the matching entity. `TNX_GPU_PICKING_FAST` enables per-frame picking at the mouse cursor position.
