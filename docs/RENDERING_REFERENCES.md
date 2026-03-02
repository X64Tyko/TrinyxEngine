# Rendering References

Curated resources for rebuilding TrinyxEngine's GPU-driven renderer on Vulkan 1.3.
Organized by implementation phase. Read in order — each phase builds directly on the last.

---

## Engine Context

The renderer lives on the **Encoder thread** (VulkRender). It owns all GPU resources,
submits command buffers, and drives the present loop. Key design constraints:

- **Vulkan 1.3 core only** — dynamic rendering, sync2, BDA all in core; no extension toggles needed.
- **VMA + ReBAR** — `VulkanMemory` wraps VMA. ReBAR path does direct CPU→VRAM writes
  (`DEVICE_LOCAL | HOST_VISIBLE`). Non-ReBAR falls back to staging. All callers use the same
  `AllocateBuffer` API with `GpuMemoryDomain` to express intent.
- **Buffer Device Address everywhere** — no descriptor sets for buffer data. Buffers are
  passed as `uint64_t` device addresses through push constants or embedded in a root
  `GpuFrameData` struct. The shader reads a single push constant (pointer to `GpuFrameData`),
  which fans out to everything else.
- **GPU-driven** — CPU submits one `DrawIndexedIndirect`. Predicate → prefix-sum → scatter
  compute passes determine what draws and writes compact `InstanceData`. The graphics pass
  reads the compacted data.
- **Slang shaders** — compiled offline via `slangc` for now; runtime compilation via the
  C++ API (`IGlobalSession` / `ISession` / `IModule`) is the target for hot-reload and
  automatic reflection of GPU structs.

---

## Phase 1 — Core Pipeline (clear → triangle → indexed cube)

**Goal**: acquire → clear swapchain → present. Then a triangle. Then a cube with hardcoded
transform. No VMA, no compute. Just the frame loop working cleanly.

### Primary tutorial
**[howtovulkan.com](https://www.howtovulkan.com/)** — Sascha Willems, 2026.
Written for engineers who already know graphics, not for beginners. Covers in one session:
- Instance / device / swapchain creation (already done in `VulkanContext`)
- Dynamic rendering (`vkCmdBeginRendering`) — no `VkRenderPass` / `VkFramebuffer`
- Synchronization2 (`vkCmdPipelineBarrier2`, `VkImageMemoryBarrier2`)
- The acquire → submit → present loop with correct fence and semaphore lifecycle
- BDA push constants instead of descriptor sets for mesh data

Cross-reference this against `VulkanContext.cpp` and `VulkanMemory.cpp` — the init code
there is already correct and you don't need to redo it.

### Frame loop reference
**[vkguide.dev — New Rendering Loop (Ch 2)](https://vkguide.dev/docs/new_chapter_2/vulkan_new_rendering/)**
The "new" vkguide is written from scratch for 1.3 (unlike the original which retrofitted
extensions). Chapter 2 sets up the exact same frame loop as `VulkRender::ThreadMain` will.
Good for verifying semaphore / fence lifecycle is correct.

### Sync2 and dynamic rendering deep dive
**[Vulkan Docs — Hello Triangle 1.3](https://docs.vulkan.org/samples/latest/samples/api/hello_triangle_1_3/README.html)**
Official Khronos sample. Use it as a reference implementation — `vkCmdPipelineBarrier2` call
signatures, `VkRenderingAttachmentInfo` layout, etc.

**[lowleveldive.com](https://lowleveldive.com/)** — Interactive side-by-side of old
`VkRenderPass` vs. `vkCmdBeginRendering`. Useful for understanding why the old API existed
and what dynamic rendering removes.

---

## Phase 2 — Memory, Staging, and ReBAR

**Goal**: Upload vertex/index buffers. Understand when to use staging vs. direct write.
Wire up the existing `VulkanMemory::AllocateBuffer` patterns correctly from VulkRender.

### The definitive ReBAR article
**[Vulkan Memory Types on PC — Adam Sawicki](https://asawicki.info/news_1740_vulkan_memory_types_on_pc_and_how_to_use_them)**
Adam Sawicki is the AMD engineer who maintains VMA. This article is the ground truth for
which memory type to use and when:
- What `DEVICE_LOCAL | HOST_VISIBLE` actually means on discrete vs. integrated
- Why write-combining (BAR) memory requires sequential writes only — **no reads, no random writes**
- The `VMA_MEMORY_USAGE_AUTO` + `HOST_ACCESS_SEQUENTIAL_WRITE` flag combination
- What changes when ReBAR is enabled in BIOS vs. the 256 MB fallback

This directly explains the design of `GpuMemoryDomain::PersistentMapped` in `VulkanMemory.cpp`.

### VMA usage patterns
**[VMA — Recommended Usage Patterns](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/usage_patterns.html)**
Reference for the `AUTO` usage + host access flag combinations. Explains the fallback chain:
ReBAR (device-local, host-visible) → DEVICE_LOCAL (GPU-only, staging upload needed).

**[VMA — Choosing Memory Type](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/choosing_memory_type.html)**
Companion page. Covers `requiredFlags`, `preferredFlags`, and `VMA_ALLOCATION_CREATE_MAPPED_BIT`
interaction — including the constraint that triggered the VMA SIGABRT:
`AUTO* + MAPPED_BIT` requires `HOST_ACCESS_SEQUENTIAL_WRITE` or `HOST_ACCESS_RANDOM`.

### ReBAR from AMD's perspective
**[AMD GPUOpen — Getting the Most out of SAM](https://gpuopen.com/learn/get-the-most-out-of-smart-access-memory/)**
Covers what upload patterns actually benefit from full ReBAR (streaming large buffers) vs.
those that don't (scattered small writes). Informs when the direct-upload path is worth using.

### Mesh upload with BDA
**[vkguide.dev — Mesh Buffers + VMA Staging](https://vkguide.dev/docs/new_chapter_3/mesh_buffers/)**
Shows VMA staging → GPU-local upload with BDA push constants instead of descriptor sets.
Close to what `UploadMeshGeometry` does in the legacy `RenderThread`. Good ground truth.

---

## Phase 3 — GPU-Driven Rendering (indirect draw, compute, compaction)

**Goal**: Replace direct `vkCmdDraw` with `vkCmdDrawIndexedIndirect`. Add compute passes
that cull entities and write compact `InstanceData`. The CPU submits one draw call regardless
of entity count.

### Architecture overview
**[vkguide.dev — GPU Driven Rendering Overview](https://vkguide.dev/docs/gpudriven/gpu_driven_engines/)**
Explains the predicate → scan → scatter model used in TrinyxEngine's compute pipeline.
MultiDrawIndirect structure, per-entity culling in compute, and why the CPU never touches
draw counts at runtime. A chapter 7 targeting Vulkan 1.3 is actively being written.

**[Dev.to — Advanced Vulkan: Frame Graph and Memory Management](https://dev.to/p3ngu1nzz/advanced-vulkan-rendering-building-a-modern-frame-graph-and-memory-management-15kn)**
Covers structuring a render thread that owns its VRAM budget and submits explicit passes.
Matches the Encoder thread model — useful when VulkRender grows a proper pass graph.

### BDA reference
**[Vulkan Docs — Buffer Device Address Sample](https://docs.vulkan.org/samples/latest/samples/extensions/buffer_device_address/README.html)**
Official Khronos sample for BDA. Shows `vkGetBufferDeviceAddress` call, the `VkBufferUsageFlags`
requirements (`SHADER_DEVICE_ADDRESS_BIT`), and reading a buffer address in a shader via
push constant. Directly relevant to how `GpuFrameData` is consumed by all shaders.

---

## Phase 4 — Bindless Descriptors and PBR

**Goal**: Drive material textures from GPU-side tables. Implement a Cook-Torrance GGX BRDF
with IBL. No per-draw descriptor binding — all material data addressed by integer index.

### PBR lighting model
**[Vulkan Docs — PBR for glTF Models](https://docs.vulkan.org/tutorial/latest/Building_a_Simple_Engine/Loading_Models/05_pbr_rendering.html)**
Official Khronos tutorial implementing Cook-Torrance GGX with glTF 2.0 material inputs
(base color, metallic-roughness, normal, occlusion, emissive). Good starting point for the
lighting model before wiring it into the SoA instance data layout.

**[SaschaWillems/Vulkan-glTF-PBR (GitHub)](https://github.com/SaschaWillems/Vulkan-glTF-PBR)**
Reference implementation with IBL (irradiance + specular prefiltered env map + BRDF LUT).
Use it as a **shader reference** — the BRDF math and IBL sampling are correct. The descriptor
binding model is old-style; replace with BDA + bindless when integrating.

### Bindless descriptor setup
**[Bindless Resources in Vulkan — Henrique Gois](https://henriquegois.dev/posts/bindless-resources-in-vulkan/)**
Step-by-step setup of `VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT` +
`UPDATE_AFTER_BIND_BIT`. This is what drives material textures from a GPU-driven pipeline
where draw calls don't know which materials they'll need at record time.

**[Vulkan Pills — Bindless Textures](https://jorenjoestar.github.io/post/vulkan_bindless_texture/)**
Companion article covering the `VkDescriptorSetLayout` flags and the shader-side
`layout(set=0, binding=0) uniform sampler2D textures[]` pattern.

**[Vulkan Docs — Descriptor Indexing Sample](https://docs.vulkan.org/samples/latest/samples/extensions/descriptor_indexing/README.html)**
Official Khronos sample. Reference for the exact extension flags combination.

### Book
**Vulkan 3D Graphics Rendering Cookbook, 2nd Edition (2025)** — Only recent book covering
bindless Vulkan end to end. Worth having as a reference for the rendering engineer. ISBN
978-1803248110.

---

## Slang C++ Runtime API

**Goal**: Replace offline `slangc` → `.spv` compilation with runtime compilation.
Enables hot-reload, reflection-driven layout generation, and per-entity shader variants
without combinatorial `.spv` explosion.

### Official documentation
**[shader-slang.org — Compilation API](https://shader-slang.org/docs/compilation-api/)**
Canonical reference for the session/module/linkage flow. The complete path is:
```
slang::createGlobalSession()        ← once at engine startup
  └─ IGlobalSession::createSession()    ← per target (Vulkan SPIR-V)
       └─ ISession::loadModule()          ← per .slang file
            └─ IModule::findEntryPointByName()
                 └─ ISession::createCompositeComponentType()   ← links modules
                      └─ IComponentType::getEntryPointCode()   ← SPIR-V blob
                           └─ vkCreateShaderModule()
```

**[Slang User Guide — Compiling Code](http://shader-slang.org/slang/user-guide/compiling)**
Covers `ISession` configuration: target profile (`spirv_1_5`), matrix layout, preprocessor
defines, and search paths for `import` resolution.

**[Slang Initialization Guide](https://shinkeys.github.io/slang-guide/)**
Concise walkthrough of the session/module/linkage flow with minimal working code.
Good starting point before reading the full API docs.

### Reflection
The Slang C++ API exposes full reflection of uniform layouts, struct fields, and entry-point
parameters via `ITypeLayout` / `IVariableLayout`. This is the mechanism that can automate
`GpuFrameData` layout generation — query field offsets from Slang at startup, assert they
match the C++ struct, or generate the C++ struct from the Slang source of truth.

### Vendored headers (in `libs/slang/include/`)
| Header | Purpose |
|---|---|
| `slang.h` | Full public API |
| `slang-com-ptr.h` | `Slang::ComPtr<T>` smart pointer |
| `slang-com-helper.h` | `SLANG_RETURN_ON_FAIL`, `SLANG_SUCCEEDED` |
| `slang-image-format-defs.h` | Format enum (required by `slang.h`) |
| `slang-deprecated.h` | Backward-compat shims (required by `slang.h`) |
| `slang-tag-version.h` | `SLANG_TAG_VERSION "2026.3.1"` |

### CMake usage
To link the runtime API from any target:
```cmake
target_link_libraries(TrinyxEngine PRIVATE Slang::Compiler)
```
`Slang::Compiler` is defined in the root `CMakeLists.txt`. Nothing currently links it;
add it when VulkRender or a `ShaderCompiler` subsystem is ready to use it.

---

## Recommended Reading Order

For a rendering engineer coming in fresh:

1. **howtovulkan.com** — get a frame on screen fast, understand the boilerplate
2. **Sawicki — Vulkan Memory Types** — understand the memory model before touching VMA
3. **vkguide.dev Ch 2–3** — frame loop + mesh upload with BDA
4. **vkguide.dev GPU Driven** — understand the compute compaction model
5. **Bindless articles** — descriptor indexing for material textures
6. **PBR references** — lighting model once the geometry pipeline is solid
7. **Slang Compilation API** — runtime shader compilation + reflection
