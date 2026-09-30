# Rendering Overview

> [Home](../Home.md) | [GPU Pipeline →](GPU-Pipeline.md)

---

## Architecture

Trinyx uses a **Visibility Buffer (VizBuffer)** architecture. The hardware rasterizer outputs a minimal 64-bit payload (InstanceID + PrimitiveID). Material evaluation, lighting, and transparency are deferred to a zero-overdraw full-screen compute pass. This integrates directly with the engine's SoA temporal ring buffers via Vulkan Buffer Device Address (BDA).

| Architecture | Flow | Key Problem |
|---|---|---|
| Traditional Deferred (G-Buffer) | Rasterize → Write Fat G-Buffer → Light | 16–32 bytes/pixel destroys VRAM bandwidth; overdraw wastes ALU |
| Industry VisBuffer (e.g. UE5 Nanite) | Rasterizer → Skinny VisBuffer → Compute Material/Light | High baseline overhead; translucency requires costly clustered forward or RT any-hit |
| Trinyx VizBuffer | HW Rasterizer → 64-bit VisBuffer → Opaque Resolve → Hybrid RT/Compute Gather → Global Radix Sort → Alpha Accumulation | Designed for worst-case VFX overdraw with bounded VRAM and early ALU exit |

---

## Current State (Shipped 2026-03)

Phase 1 of the full pipeline is operational:

- **Raw Vulkan** — volk 1.4.304 + VMA 3.3.0 (`vk::raii::`)
- **Slang shaders** — compute: `predicate`, `prefix_sum`, `scatter`, `build_draws`, `sort_instances`, `skinning`; graphics: `cube.vert`, `cube.frag`, viewport gradient
- **Buffer Device Address** — `GpuFrameData` struct holds all BDAs; no per-frame descriptor set updates
- **5-pass compute pipeline** — predicate → prefix_sum → scatter → build_draws → sort_instances (see [GPU Pipeline](GPU-Pipeline.md))
- **Per-mesh indirect draws** — one `DrawIndexedIndirect` command per mesh type, instances sorted by mesh
- **GPU skinning** — compute skinning for skeletal entities with indirect dispatch
- **Shading** — a single directional light (Lambert). No textures, materials, shadows, or post-processing yet
- **5 PersistentMapped field slabs** — cycle independently from 2 GPU frame-in-flight slots, decoupling Logic from VSync
- **Dirty-bit-driven partial upload** — only modified entities uploaded per frame (see [Dirty Bit Upload](Dirty-Bit-Upload.md))

---

## Full Target Pipeline (6 Phases)

### Phase 1 — Compute Culling & Predicate

`predicate.slang` acts as a task shader equivalent: evaluates frustum culling and tests entity bounds against the t-1 Hierarchical Z-Buffer (HZB), then outputs indirect draw commands for opaque and alpha-tested geometry.

*Currently implemented:* active-flag predication only. Frustum culling and HZB test are pending.

### Phase 2 — Raster & HZB Generation

Graphics queue opaque pass: Early-Z minimalist rasterization, HZB build, late culling pass for newly-occluded objects. Outputs a `R32G32_UINT` VisBuffer and depth target. Alpha-tested foliage uses the pre-built depth to guarantee Early-Z hardware culling.

### Phase 3 — Material Resolve & Light Grid

**Light build:** A compute pre-pass bins dynamic lights into a 3D Froxel Grid / Screen-Space Cascade Grid.

**Material evaluation:** Full-screen compute reads the 64-bit VisBuffer, fetches BDA vertices, reconstructs barycentrics analytically, applies Compute-Based Variable Rate Shading (VRS) via subgroup shuffle (2×2 quads with matching PrimitiveID share one material evaluation), and writes the lit opaque HDR target.

*Also planned here:* True sub-frame motion blur — evaluating Bézier curves from t−8 to t using the 512Hz logic history for cinematic curved blur.

### Phase 4 — Hybrid Transparency Gather

Two sources feed the same `GlobalPixelQueue` SSBO:

- **Volumetrics (Source A):** Smoke, fire, and particles use `atomicAdd` to push into the queue. A strict per-pixel atomic 9-layer limit hard-caps VRAM use and immediately drops occluded particles.
- **RT solids (Source B):** A full-screen compute shader fires inline ray queries (`OpRayQueryKHR`) to resolve glass, water, and refractive surfaces. Hit data is packed into the same queue — no Any-Hit shader stalls on volumetrics.

### Phase 5 — Global Radix Sort & Mega-Dispatch

**Sort:** GPU Radix Sort on 64-bit keys: `[Tile ID (16) | ReverseDepth (32) | Payload (16)]`. The tile prefix groups threads into 32×32 screen tiles for ~90% L1/L2 cache hit rate during the final dispatch.

**Dispatch:** A single indirect dispatch evaluates the sorted queue, blends materials front-to-back, and exits threads early once a pixel's accumulated alpha reaches 0.99 — saving ALU on occluded smoke layers.

### Phase 6 — Post-Process & Reconstruction

TAA, native DLSS/FSR 3 reconstruction (1080p internal → 4K output), and motion blur using render-to-render motion vectors from the decoupled HistorySlab.

---

## Core Constraints

| Constraint | Reason |
|---|---|
| **64-bit VisBuffer** — strictly 64 bits | Minimize VRAM bandwidth. Barycentrics and normals reconstructed analytically. |
| **64-bit transparency sort key** | Front-to-back ordering + spatial grouping for cache locality. |
| **Inline RT only** — `OpRayQueryKHR` | No Any-Hit shaders; they stall the pipeline on volumetric geometry. |
| **Atomic transparency limit** — 9 layers per pixel | Prevents unbounded VRAM growth under heavy VFX. |
| **BDA everywhere** | No per-frame descriptor set updates. All slab data via Buffer Device Address. |
| **Unified GI / audio cascades** | The same Cascaded Lighting Grid drives GPU Global Illumination and CPU-side Physically Based Audio propagation. |

---

## Transparency Performance Targets (1440p)

Transparency frame budget goal: **< 4.0 ms** within a 16.6 ms frame.

| Scenario | Naive Scaling | VizBuffer Behavior |
|---|---|---|
| Heavy RT solids (glass city) | VRAM/ALU scales linearly with overdraw | O(1) CPU, O(N) GPU RT — BVH traversal on RT cores |
| 10× volumetric overdraw (smoke) | VRAM bandwidth explodes; ROPs saturate | Bounded VRAM via atomic gather limit; alpha early-exit terminates occluded ALU lanes |
| Interwoven solids + volumetrics (boss fight) | Depth sort fails across passes or RT Any-Hit stalls | O(N log N) sort with high cache coherency from tiled sort key |

---

## Remaining Work

| Item | Status |
|---|---|
| Frustum culling + HZB test in predicate pass | Pending |
| Texture pipeline (import, compression, mips, streaming) | Not yet designed |
| Material model and authoring | Not yet designed |
| Shadows | Not yet designed |
| Tonemapping and post-process stack | Not yet designed |
| State-sorted rendering (64-bit sort keys, GPU radix sort) | Designed |
| VizBuffer raster pass + HZB generation | Designed |
| Material resolve compute pass | Designed |
| Hybrid transparency gather (Phase 4) | Designed |
| Global radix sort + mega-dispatch (Phase 5) | Designed |
| TAA / DLSS / FSR 3 reconstruction | Pending |
| Slang runtime compilation for hot-reload | Pending |
