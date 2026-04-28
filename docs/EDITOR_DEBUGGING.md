# Trinyx Engine: Editor Debugging & Tooling Suite

> **Navigation:** [← Back to README](../README.md) | [Architecture →](ARCHITECTURE.md)

## Overview
The Trinyx Engine debugging suite bridges the gap between high-performance Data-Oriented Design (DoD) and developer-friendly Object-Oriented Programming (OOP). It provides real-time, interactive windows into the engine's contiguous memory arenas, temporal ring buffers, multi-threaded task scheduler, network rollback state, GPU pipeline, and asset streaming systems.

This document serves as the master blueprint for all engine tooling, available to be implemented a-la-carte as project needs dictate.

---

## Part 1: Memory & Temporal Visualization

### 1.1 The Slab Heatmap (Compute-Driven Memory View)
A GPU-generated texture visualizing the entire Volatile and Temporal SoA slab with zero CPU overhead during normal execution. Rendered via a Slang compute shader (`slab_visualizer.slang`) reading from the active `InstanceBuffer`.
* **Layout:** X-Axis represents Entities (wrapped into blocks). Y-Axis represents Fields and Time (Field-Major layout, stacking history frames `T` through `T-N` vertically per field to visualize temporal "streaks").
* **Zoom Levels:**
  * *Micro (2x2 px/cell):* Exact data representation. Hovering triggers CPU-side reverse lookups.
  * *Macro (5px/64 entities):* Uses `popcount()` on the 64-bit `TemporalFlags` bitplane. Visualizes macro-gaps (black), fragmented chunks (dim green), and packed memory (bright green). Instantly reveals partition boundaries (`RENDER`, `DUAL`, `PHYS`, `LOGIC`).
* **Chunk Overlay:** The GPU draws borders and fullness gradients for AoS chunk allocations over the SoA heatmap using a `ChunkMeta` SSBO.

### 1.2 DoD-to-OOP Side-Table
To keep SIMD sweeps pure, Entities do not store `Owner*` pointers in the slab. 
* **Architecture:** `ConstructRegistry` maintains a lock-free `FlatMap<EntityCacheIndex, ConstructMetadata>`.
* **Usage:** When hovering over an anonymous pixel in the heatmap, the Editor queries the Side-Table to display the owning Construct's RTTI name and View name (e.g., *"BarrelAssembly owned by Turret_3"*).

### 1.3 Deep Temporal Scrubbing
* **Editor Ring Depth:** When `TNX_ENABLE_EDITOR` is defined, `EngineConfig::TemporalFrameCount` overrides to a deep buffer (e.g., 256 frames / 0.5s at 512Hz) to allow human-readable timeline debugging.
* **Full Rebuild:** Scrubbing the paused timeline performs a blocking CPU-to-GPU transfer of the requested `HistorySlab` frame, updating the Heatmap to accurately display the past.

### 1.4 Play-From-Here Resimulation
Allows developers to edit history and watch the timeline heal.
1. Scrub to `T-4` and edit a value in the Details Panel.
2. Press **Propagate Resim**.
3. The engine restores the Jolt Physics snapshot for `T-4` and fast-forwards the Brain thread 4 ticks.
4. The `PresentationReconciler` intercepts and suppresses all `TemporalEvents` during the fast-forward to prevent audiovisual spam.

---

## Part 2: Spatial & Physics Debugging

### 2.1 The Command-Based Debug Draw API
A thread-safe, triple-buffered command ring buffer for drawing transient 3D shapes.
* **Workflow:** Logic code (512Hz) pushes commands with a `Duration` parameter. 
* **Rendering:** The Encoder thread consumes the buffer, converting `Fixed32` cell-relative coordinates to `Float32` camera-relative coordinates, issuing a batched graphics draw call.

### 2.2 Post-Process Non-Visibility Overlays
For debugging spatial systems that aren't strictly visual (Physically Based Audio, AI perception, Trigger Volumes) without fighting the depth buffer.
* **Architecture:** The compute culling pass flags the instance payload (e.g., `DebugPBAActive`). 
* **Execution:** A post-process pass reads the `InstanceBuffer` and the Scene Depth, projecting the bounds of flagged entities and drawing translucent debug shaders directly on top of the rendered scene (x-ray behavior).

### 2.3 Spatial Manipulation (ImGuizmo)
Integration of `ImGuizmo` for interactive Translate, Rotate, and Scale operations within the PIE viewport. Edits translate directly to the `EntityRecord`'s `CTransform` data (respecting the currently scrubbed temporal frame).

---

## Part 3: Performance & Profiling

### 3.1 Tracy Profiler Integration
Native integration of Tracy for CPU-side frame profiling, tracking the "Trinyx Trinity" (Sentinel, Brain, Encoder threads), and visualizing lock contention on the Job System MPMC ring buffers.

### 3.2 System Execution & Job Graph Visualizer
A node-based dependency graph representing the actual scheduling logic of the Brain thread.
* **Functionality:** Visualizes the critical path of `PrePhys`, `Physics`, `PostPhys`, and `ScalarUpdate` batches. Helps developers identify if a specific Construct's tick registration is forming a pipeline bubble or forcing a thread sync.

### 3.3 Job Queue Heatmap
An in-editor visualizer for the four priority queues (Logic, Render, Physics, General), displaying active worker core assignments and task-stealing rates.

---

## Part 4: Network & Architecture Tooling

### 4.1 Network Condition Simulator
Built directly into the `NetConnectionManager` to inject artificial latency, packet loss, and jitter, allowing local stress-testing of the `ReplicationSystem` and rollback artifacting.

### 4.2 Net Packet & Bandwidth Profiler
A deep-inspection tool for GameNetworkingSockets (GNS) payloads.
* **Visualization:** A scrolling timeline showing outgoing/incoming packet sizes.
* **Payload Breakdown:** Clicking a packet reveals exactly how many bytes were spent on `EntitySpawns` vs `StateCorrections`, and which specific components are eating the most bandwidth. Crucial for tuning Delta Compression and Interest Management.

### 4.3 Registry Health & ABA Debugger
* **Metrics:** Tracks `AllocatedEntityCount` vs. `TotalEntityCount`, alongside active visualizations of the `PendingDestructions`, `PendingLocalRecycles`, and `PendingNetRecycles` queues to hunt down ABA aliasing bugs.
* **Defrag Triggers:** Manual buttons to force defragmentation and observe slab compaction in real-time.

### 4.4 Deterministic Input Replay (The Bug Catcher)
Leverages the engine's fixed-point math and strict fixed-timestep to record and replay exact scenarios.
* **Functionality:** Records user inputs and RNG seeds to a file. Developers can load this file to play back a sequence frame-for-frame, guaranteeing 100% reproduction of physics or logic bugs.

### 4.5 Flow Graph Visualizer
A real-time tree view of the `FlowManager`, displaying the active `GameState` stack, `GameMode`, and all instantiated `Constructs` grouped by their lifetime scope (Persistent, Session, World, Level) to catch memory leaks across travel transitions.

---

## Part 5: Render & GPU Debugging

### 5.1 G-Buffer & Viz-Buffer Inspectors
A dropdown in the PIE viewport that modifies the composite shader to output intermediate attachments (Base Color, World Normals, Depth, Motion Vectors) instead of the final lit frame.

### 5.2 Shader Complexity & Overdraw Heatmap
Swaps standard fragment shaders with an additive blend pipeline. Pixels accumulate color from Green → Yellow → Red → White, highlighting expensive transparent geometry and fill-rate bottlenecks.

### 5.3 1-Click RenderDoc Capture
A toolbar button that dynamically loads the RenderDoc In-Application API, capturing exactly one Vulkan frame and auto-launching the RenderDoc UI for deep shader and BDA inspection.

### 5.4 GPU-Driven Culling Visualizer
Debugs the `predicate` -> `prefix_sum` -> `scatter` compute pipeline.
* **Execution:** Developers can freeze the frustum and fly the camera outside of it. The compute pipeline flags culled entities in the `InstanceBuffer` payload instead of dropping them, allowing the fragment shader or post-process pass to render them in a translucent "error" state.

---

## Part 6: Core Systems & Asset Tooling

### 6.1 Asset Dependency Graph
A node-based viewer to hunt down VRAM and RAM bloat.
* **Functionality:** Shows the exact tree of references (e.g., `PlayerConstruct` → `CharacterMesh` → `HeroMaterial` → `Albedo_8K.png`). Allows developers to easily spot unreferenced assets or accidental hard-references that break asynchronous streaming.

### 6.2 VRAM Fragmentation Analyzer (VMA Integration)
Leverages Vulkan Memory Allocator (VMA) via `vmaBuildStatsString`. Parses the internal JSON to display a block-by-block visual map of physical VRAM, highlighting fragmented free space.

### 6.3 Global CVar (Console Variable) System
A Quake-style drop-down console overlay.
* **Usage:** Allows fast, runtime tweaking of engine and gameplay variables (`r.ShadowQuality`, `phys.Gravity`, `net.SimulatedPing`) without needing to compile custom editor UI for every property.

---

## Part 7: Trinyx-Specific Subsystems

### 7.1 Fixed-Point Precision Inspector
Visualizes the Quantization Error introduced when converting Logic `Fixed32` coordinates to Render `Float32` coordinates. Highlights entities in a heat gradient based on precision lost, aiding in diagnosing visual-only jitter.

### 7.2 Anti-Event Stress Tester
An editor toggle to force artificial presentation mispredictions. The engine intentionally delays presentation evaluation, forcing the `PresentationReconciler` to generate "Anti-Events" (rapid fades/decays) to test audio and VFX graceful degradation curves without a live networked session.

### 7.3 Jolt Sleep/Awake Heatmap
An overlay coloring physical entities based on their Jolt activation state (Awake = White, Sleeping = Dark Gray). Essential for diagnosing missed explicit awakenings at the `JoltJobSystemAdapter` boundary.
