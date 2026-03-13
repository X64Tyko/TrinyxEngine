# GPU-Driven Rendering Architecture Design

**Date:** 2026-02-26 (design), updated 2026-03-12
**Status:** Phase 1 Complete — GPU compute pipeline operational (Slang + BDA)
**Goal:** Transition from CPU-side interpolation to full GPU-driven rendering pipeline

---

## Executive Summary

Moving from CPU-driven to GPU-driven rendering to:
- Eliminate bandwidth bottleneck (currently ~30μs improvement at 100k entities is memory-bound)
- Leverage existing SoA temporal cache structure (already GPU-friendly)
- Scale to millions of entities
- Enable future features (culling, LOD, streaming)
- Align with modern engine architecture (2025 baseline)

---

## Current Architecture

### Data Flow (CPU-Driven)
```
LogicThread (60Hz)
  └─ Writes temporal frame T+1 to TemporalComponentCache (SoA layout)

RenderThread (144Hz)
  ├─ Locks frames T and T-1 for read
  ├─ Interpolates 100k entities (SIMD, but memory bandwidth bound)
  ├─ Writes to InstanceData (AoS) in TransferBuffer
  ├─ CopyPass: TransferBuffer → InstanceBuffer
  ├─ RenderPass: DrawIndexedInstanced
  └─ Unlocks frames
```

### Problems
1. **Bandwidth bottleneck:** Reading 18 SoA arrays (9 prev + 9 curr) per frame
2. **CPU compute waste:** Interpolation blocks render thread
3. **Double upload:** CPU→Transfer, Transfer→Instance
4. **Tight coupling:** Can't add GPU culling without CPU round-trip

### What Works Well
- ✅ Temporal cache SoA layout (perfect for GPU upload)
- ✅ Lock-free frame access
- ✅ Separate logic/render threads
- ✅ Frame interpolation for smooth motion

---

## Target Architecture (GPU-Driven)

### Data Flow (GPU-Driven)
```
LogicThread (60Hz)
  └─ Writes temporal frame T+1 to TemporalComponentCache (SoA layout)

RenderThread (144Hz)
  ├─ Uploads frame T and T-1 as SSBOs (once per logic frame, not per render frame)
  ├─ Updates camera/view uniforms
  ├─ Dispatches compute shaders:
  │   ├─ Culling (optional, future)
  │   ├─ LOD selection (optional, future)
  │   └─ Interpolation + Compaction
  ├─ Memory barriers
  ├─ RenderPass: DrawIndexedIndirect (count from compute)
  └─ Submit

GPU (Async)
  ├─ Compute: Interpolate + compact active entities
  ├─ Compute: (Future) Frustum culling, occlusion
  └─ Vertex: Transform, rasterize
```

### Benefits
- ✅ **50-75% less bandwidth:** Upload raw frames once, not interpolated instances every frame
- ✅ **Zero CPU interpolation cost:** GPU does it during vertex fetch latency
- ✅ **Scales to millions:** GPU parallel across all entities
- ✅ **Future-proof:** Enables GPU culling, streaming, indirect rendering
- ✅ **Simpler render thread:** Orchestrates GPU work instead of heavy CPU compute

---

## Technical Design

### Phase 1: GPU Interpolation (Minimal Change)

#### GPU Buffer Layout
```cpp
// Upload from TemporalComponentCache (already SoA!)
struct TransformSoA {
    float posX[MAX_ENTITIES];
    float posY[MAX_ENTITIES];
    float posZ[MAX_ENTITIES];
    float rotX[MAX_ENTITIES];
    float rotY[MAX_ENTITIES];
    float rotZ[MAX_ENTITIES];
    float scaleX[MAX_ENTITIES];
    float scaleY[MAX_ENTITIES];
    float scaleZ[MAX_ENTITIES];
};

struct ColorSoA {
    float r[MAX_ENTITIES];
    float g[MAX_ENTITIES];
    float b[MAX_ENTITIES];
    float a[MAX_ENTITIES];
};

struct FlagsSoA {
    int32_t flags[MAX_ENTITIES];  // Bit 31 = active
};
```

#### Compute Shader (Interpolation + Compaction)
```glsl
#version 450
layout(local_size_x = 256) in;

// Input: Temporal frames (SoA)
layout(std430, binding = 0) readonly buffer TransformsPrev { TransformSoA prev; };
layout(std430, binding = 1) readonly buffer TransformsCurr { TransformSoA curr; };
layout(std430, binding = 2) readonly buffer ColorsCurr { ColorSoA color; };
layout(std430, binding = 3) readonly buffer FlagsCurr { int flags[]; };

// Output: Compacted instances (AoS)
layout(std430, binding = 4) writeonly buffer Instances { InstanceData instances[]; };

// Atomic counter for compaction
layout(std430, binding = 5) buffer VisibleCount { uint count; };

// Uniforms
layout(push_constant) uniform PushConstants {
    float alpha;
    uint maxEntities;
};

const uint ACTIVE_MASK = 0x80000000u;

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= maxEntities) return;

    // Early exit if not active
    if ((flags[idx] & ACTIVE_MASK) == 0u) return;

    // Interpolate transform
    vec3 pos = mix(
        vec3(prev.posX[idx], prev.posY[idx], prev.posZ[idx]),
        vec3(curr.posX[idx], curr.posY[idx], curr.posZ[idx]),
        alpha
    );

    vec3 rot = mix(
        vec3(prev.rotX[idx], prev.rotY[idx], prev.rotZ[idx]),
        vec3(curr.rotX[idx], curr.rotY[idx], curr.rotZ[idx]),
        alpha
    );

    vec3 scale = mix(
        vec3(prev.scaleX[idx], prev.scaleY[idx], prev.scaleZ[idx]),
        vec3(curr.scaleX[idx], curr.scaleY[idx], curr.scaleZ[idx]),
        alpha
    );

    vec4 col = vec4(color.r[idx], color.g[idx], color.b[idx], color.a[idx]);

    // Atomic compaction - write to output buffer
    uint outIdx = atomicAdd(count, 1u);
    instances[outIdx].position = vec4(pos, 0.0);  // Pad to vec4 for alignment
    instances[outIdx].rotation = vec4(rot, 0.0);
    instances[outIdx].scale = vec4(scale, 0.0);
    instances[outIdx].color = col;
}
```

#### RenderThread Changes
```cpp
void RenderThread::RenderFrame() {
    TNX_ZONE_N("RenderFrame");

    // 1. Get latest frame from logic
    uint32_t frame = LogicPtr->GetLastCompletedFrame();
    uint32_t prevFrame = TemporalCache->GetPrevFrame(frame);

    // 2. Upload temporal frames if changed (once per logic tick, not per render frame!)
    if (frame != LastUploadedFrame) {
        UploadTemporalFrame(frame, prevFrame);
        LastUploadedFrame = frame;
    }

    // 3. Update camera uniforms
    UpdateCameraUniforms(CurrentFrameHeader);

    // 4. Calculate interpolation alpha
    float alpha = CalculateInterpolationAlpha();

    // 5. Request GPU resources
    RequestGPUResources();
    WaitForCommandBuffer();

    // 6. Dispatch compute shader
    DispatchInterpolationCompute(alpha, entityCount);

    // 7. Barrier (compute write → vertex read)
    InsertComputeToVertexBarrier();

    // 8. Render pass (draw indirect)
    WaitForSwapchainTexture();
    RecordDrawPass();

    // 9. Submit
    SignalReadyToSubmit();
}
```

### Phase 2: GPU Culling (Future)

Add frustum culling compute shader before interpolation:
```glsl
// Culling pass: sets flags[idx] inactive if outside frustum
// Interpolation pass: skips inactive entities (already handled!)
```

### Phase 3: Full Indirect Rendering (Future)

Replace `DrawIndexedInstanced` with `DrawIndexedIndirect`:
```cpp
// Compute shader writes draw commands directly
struct DrawIndirectCommand {
    uint indexCount;
    uint instanceCount;  // Written by compute atomicAdd
    uint firstIndex;
    uint vertexOffset;
    uint firstInstance;
};
```

---

## Implementation Plan

### Milestone 1: Compute Interpolation (1-2 weeks)
**Goal:** Replace CPU SIMD loop with compute shader

1. **Create GPU buffers for temporal frames**
   - `SSBOTransformPrev`, `SSBOTransformCurr` (upload from TemporalCache)
   - `SSBOColorCurr`, `SSBOFlags`
   - `SSBOInstances` (output)
   - `SSBOVisibleCount` (atomic counter)

2. **Write interpolation compute shader**
   - Input: SoA temporal frames
   - Output: AoS InstanceData
   - Atomic compaction for active entities

3. **Refactor RenderThread::InterpolateTemporalFrames()**
   - Remove SIMD loop (lines 546-680 in RenderThread.cpp)
   - Add `UploadTemporalFrame()` - memcpy SoA to SSBO
   - Add `DispatchInterpolationCompute()` - dispatch shader
   - Add compute→vertex barrier

4. **Test & benchmark**
   - Verify same visual output
   - Measure bandwidth savings
   - Profile with RenderDoc

### Milestone 2: Optimize Upload (1 week)
**Goal:** Minimize upload bandwidth

1. **Delta compression (optional)**
   - Only upload changed entities
   - Track dirty masks in TemporalCache

2. **Persistent mapped buffers**
   - Avoid copy overhead
   - Triple-buffer for frames in flight

3. **Benchmark**
   - Target: <10μs upload time at 100k entities

### Milestone 3: GPU Culling (2-3 weeks)
**Goal:** Add frustum culling compute pass

1. **Culling compute shader**
   - Input: entity bounds + camera frustum
   - Output: clear inactive flags

2. **Integrate into pipeline**
   - Dispatch before interpolation
   - Share flags buffer

3. **Benchmark**
   - Should handle 1M entities easily

### Milestone 4: Indirect Rendering (1-2 weeks)
**Goal:** Fully GPU-driven draw calls

1. **DrawIndirect buffer**
   - Compute writes instance count
   - GPU dispatches draw without CPU

2. **Remove CPU instance count tracking**

---

## Data Structure Changes

### TemporalComponentCache (No Changes!)
Already perfect for GPU upload:
```cpp
// Existing code already returns contiguous SoA arrays
float* posX = TemporalCache->GetFieldData(header, xformType, 0, allocSize);
// Just upload this pointer directly to SSBO!
```

### RenderThread (Simplifications)
```cpp
class RenderThread {
    // REMOVE:
    // - InterpBufferCapacity, InterpBufferCount (GPU manages this now)
    // - TransferBuffer (direct SSBO upload)
    // - CPU interpolation SIMD code

    // ADD:
    SDL_GPUBuffer* SSBOTransformPrev;
    SDL_GPUBuffer* SSBOTransformCurr;
    SDL_GPUBuffer* SSBOColorCurr;
    SDL_GPUBuffer* SSBOFlags;
    SDL_GPUBuffer* SSBOInstances;      // Output from compute
    SDL_GPUBuffer* SSBOVisibleCount;    // Atomic counter
    SDL_GPUComputePipeline* InterpolationPipeline;

    uint32_t LastUploadedFrame = 0;  // Avoid re-uploading same frame
};
```

### InstanceData (No Changes)
Keep AoS output format for GPU compatibility:
```cpp
struct alignas(16) InstanceData {
    float PositionX, PositionY, PositionZ, _pad0;
    float RotationX, RotationY, RotationZ, _pad1;
    float ScaleX, ScaleY, ScaleZ, _pad2;
    float ColorR, ColorG, ColorB, ColorA;
};
```

---

## Performance Expectations

### Current (CPU-Driven)
- 100k entities: ~30μs interpolation (memory bound)
- Bandwidth: ~200MB/frame (read 18 arrays + write instances)
- Scales poorly: 1M entities = ~300μs+

### Target (GPU-Driven)
- 100k entities: <5μs upload + GPU compute (overlapped with prev frame)
- Bandwidth: ~50MB/frame (upload raw frames once per logic tick)
- Scales well: 1M entities = ~10μs upload, GPU handles rest

**Expected improvement:** 5-10x at 100k entities, 50x+ at 1M entities

---

## Open Questions / Design Decisions

### 1. Buffer Management Strategy
**Option A:** Ring buffer (3 frames in flight)
- Pro: Maximizes GPU utilization
- Con: More complex sync

**Option B:** Double buffer (2 frames)
- Pro: Simpler
- Con: May stall occasionally

**Decision:** Start with double buffer, profile, upgrade if needed

### 2. Upload Strategy
**Option A:** Upload full frames every logic tick
- Pro: Simple
- Con: Wastes bandwidth if few entities changed

**Option B:** Delta upload (only changed entities)
- Pro: Minimal bandwidth
- Con: Complex tracking, GPU needs indirection table

**Decision:** Start with full upload, optimize later if profiling shows it's a bottleneck

### 3. Atomic Compaction vs Pre-Scan
**Option A:** Atomic compaction (current design)
- Pro: Simple, one-pass
- Con: Atomic contention at high entity counts

**Option B:** Two-pass (scan + compact)
- Pro: Better scaling
- Con: More complex, extra dispatch

**Decision:** Start with atomic, switch to scan if >1M entities show contention

### 4. Shader Language
**Option A:** GLSL
- Pro: Cross-platform (via SPIRV)
- Con: Less tooling than HLSL

**Option B:** HLSL
- Pro: Better tooling, familiar for many
- Con: Requires SPIRV cross-compile

**Option C:** Slang

- Pro: C++-like syntax, reflection API, import system, compiles to SPIRV
- Con: Newer ecosystem

**Decision:** Slang — adopted 2026-03. 5 shaders in `shaders/`, compiled offline via `slangc`.
Runtime compilation via C++ API planned for hot-reload.

---

## Testing Strategy

### Validation
1. **Visual comparison:** Render side-by-side (CPU vs GPU path)
2. **Determinism test:** Same frame number = identical output
3. **Stress test:** 1M entities, verify no corruption

### Performance Benchmarks
1. **Upload time:** Measure `UploadTemporalFrame()`
2. **Compute time:** GPU timestamp queries
3. **Total frame time:** Compare old vs new
4. **Bandwidth:** GPU performance counters

### Debugging Tools
- **RenderDoc:** Inspect buffers, shader execution
- **NSight:** Profile GPU performance
- **Fallback:** Keep CPU path as debug mode for 1-2 versions

---

## Migration Path

### Week 1-2: Foundation
- [ ] Create GPU buffer abstractions
- [ ] Write interpolation compute shader
- [ ] Implement `UploadTemporalFrame()`

### Week 3: Integration
- [ ] Replace CPU interpolation with compute dispatch
- [ ] Add barriers and sync
- [ ] Test with small entity counts

### Week 4: Validation
- [ ] Visual regression tests
- [ ] Performance benchmarks
- [ ] Fix bugs

### Week 5+: Optimization
- [ ] Profile with RenderDoc
- [ ] Optimize upload strategy
- [ ] Add culling compute pass

---

## References

### Similar Engines
- **Unreal 5 Nanite:** Full GPU-driven pipeline
- **Unity DOTS:** Compute-based rendering
- **Frostbite:** GPU culling + indirect rendering

### Resources
- Siggraph 2015: "GPU-Driven Rendering Pipelines" (Wihlidal)
- GDC 2016: "Optimizing the Graphics Pipeline with Compute" (Vlachos)
- [GPU Gems 3: Chapter 29](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-29-real-time-rigid-body-simulation-gpus)

---

## Implementation Notes

**What shipped (2026-03):**

- Migrated from SDL3 GPU backend to raw Vulkan (volk 1.4.304 + VMA 3.3.0, vk::raii::)
- Slang shaders replaced GLSL: predicate, prefix_sum, scatter, cube.vert, cube.frag
- Buffer Device Address (BDA) replaces descriptor sets — GpuFrameData struct holds all BDAs
- 3-pass compute: predicate → prefix_sum (Option-B subgroup scan) → scatter (GPU interpolation + InstanceBuffer SoA)
- 5 PersistentMapped field slabs cycle independently of 2 GPU frame-in-flight slots
- `DrawIndexedIndirect` driven by scatter pass `DrawArgs.instanceCount`
- Full-slab copy per new logic frame; dirty-bit-driven partial upload is next

**Remaining work:**

- Wire cumulative dirty bit array to GPU upload path (partial upload)
- Frustum culling compute pass
- State-sorted rendering (64-bit sort keys, GPU radix sort)
- Slang runtime compilation for hot-reload
