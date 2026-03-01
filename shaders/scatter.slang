// scatter.slang — Pass 3 of 3.
//
// For each active entity i, interpolates all temporal fields between their
// previous and current frame values and scatters the results into the compact
// SoA InstanceBuffer at the output index supplied by the prefix sum pass.
//
// Also writes DrawArgs.instanceCount from the finalised compact counter, so
// the subsequent DrawIndexedIndirect draws exactly the right number of cubes.
//
// SoA InstanceBuffer layout:
//   field slot k, entity outIdx  →  InstancesAddr[ k * OutFieldStride + outIdx ]
//   where k = GpuFieldSemantic - 1  (PosX=0 .. ColorA=12)

#include "GpuFrameData.slang"

[[vk::push_constant]]
cbuffer PushConstants { uint64_t gFrameDataAddr; }

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID)
{
    GpuFrameData* fd = (GpuFrameData*)gFrameDataAddr;

    // Thread 0 copies the finalised compact count into DrawArgs.instanceCount.
    // CompactCounterAddr is fully written by all prefix-sum workgroups before this
    // dispatch (pipeline barrier between prefix_sum and scatter dispatches).
    // VkDrawIndexedIndirectCommand layout: { indexCount, instanceCount, ... }
    // instanceCount is uint32 at byte-offset 4 → uint index 1.
    if (gid.x == 0u)
    {
        uint compactCount = ((uint*)fd->CompactCounterAddr)[0];
        ((uint*)fd->DrawArgsAddr)[1] = compactCount;
    }

    uint i = gid.x;
    if (i >= fd->EntityCount)
        return;

    // Re-check active flag (predicate may have been 0 even if tid < entityCount).
    uint flags = ((uint*)fd->FlagsAddr)[i];
    if ((flags >> 31u) == 0u)
        return;

    uint outIdx = ((uint*)fd->ScanAddr)[i];
    float alpha  = fd->Alpha;
    uint  stride = fd->OutFieldStride;
    float* outBuf = (float*)fd->InstancesAddr;

    // Interpolate every registered temporal field and write to the output slot
    // determined by its GpuFieldSemantic.  Supports only 4-byte (float) fields;
    // larger types (e.g. Matrix4) are silently skipped for now.
    uint fieldCount = fd->FieldCount;
    for (uint f = 0u; f < fieldCount; ++f)
    {
        uint sem = fd->FieldSemantics[f];
        uint esz = fd->FieldElementSize[f];

        if (sem == kSemGeneric || sem > kGpuOutFieldCount)
            continue;   // Generic / out-of-range: not mapped to an output slot

        if (esz != 4u)
            continue;   // Only float scalar fields for now

        float prev   = ((float*)fd->PrevFieldAddrs[f])[i];
        float curr   = ((float*)fd->CurrFieldAddrs[f])[i];
        float lerped = prev + (curr - prev) * alpha;

        // Field slot = semantic - 1  (PosX semantic 1 → slot 0, ColorA semantic 13 → slot 12)
        outBuf[(sem - 1u) * stride + outIdx] = lerped;
    }
}
