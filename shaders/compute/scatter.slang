// scatter.slang — Pass 3 of 5.
//
// For each active entity i, interpolates all temporal fields between their
// previous and current frame values and scatters the results into the compact
// SoA UnsortedInstances buffer at the output index supplied by the prefix sum.
//
// Also reads MeshID and atomically increments MeshHistogram[meshID] for the
// counting sort in the subsequent build_draws + sort_instances passes.
//
// SoA UnsortedInstances layout:
//   field slot k, entity outIdx  →  UnsortedInstancesAddr[ k * OutFieldStride + outIdx ]
//   where k = GpuFieldSemantic - 1  (PosX=1 .. MeshID=15)
//
// Quaternion fields (RotQx..RotQw) are lerped per-component here.
// The vertex shader normalizes the result (nlerp) to correct for
// interpolation drift.  This is cheaper than slerp and sufficient
// for consecutive frames at 512Hz.

#include "common/GpuFrameData.slang"

[[vk::push_constant]]
cbuffer PushConstants { uint64_t gFrameDataAddr; }

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID)
{
    GpuFrameData* fd = (GpuFrameData*)gFrameDataAddr;

    uint i = gid.x;
    if (i >= fd->EntityCount)
        return;

    // Re-check active flag (predicate may have been 0 even if tid < entityCount).
    // Flags live in the field slab at CurrFieldAddrs[0] (SemFlags, always index 0).
    uint flags = ((uint*)fd->CurrFieldAddrs[0])[i];
    if ((flags >> 31u) == 0u)
        return;

    uint outIdx = ((uint*)fd->ScanAddr)[i];
    float alpha  = fd->Alpha;
    uint  stride = fd->OutFieldStride;
    float* outBuf = (float*)fd->UnsortedInstancesAddr;

    // Interpolate every registered temporal field and write to the output slot
    // determined by its GpuFieldSemantic.
    uint fieldCount = fd->FieldCount;
    for (uint f = 0u; f < fieldCount; ++f)
    {
        uint sem = fd->FieldSemantics[f];
        uint esz = fd->FieldElementSize[f];

        if (sem == SemGeneric || sem == SemFlags)
            continue;   // Flags are uint, not interpolatable

        if (sem > GpuOutFieldCount)
            continue;   // Out-of-range

        // MeshID: uint field, no interpolation — copy current value and build histogram
        if (sem == SemMeshID)
        {
            uint meshID = ((uint*)fd->CurrFieldAddrs[f])[i];
            outBuf[(sem - 1u) * stride + outIdx] = asfloat(meshID);

            // Atomically increment mesh histogram for counting sort
            uint* histogram = (uint*)fd->MeshHistogramAddr;
            InterlockedAdd(histogram[meshID], 1u);
            continue;
        }

        if (esz != 4u)
            continue;   // Only float scalar fields for now

        float prev   = ((float*)fd->PrevFieldAddrs[f])[i];
        float curr   = ((float*)fd->CurrFieldAddrs[f])[i];
        float lerped = prev + (curr - prev) * alpha;

        // Field slot = semantic - 1  (PosX semantic 2 → slot 1, ColorA semantic 15 → slot 14)
        outBuf[(sem - 1u) * stride + outIdx] = lerped;
    }

#ifdef TNX_GPU_PICKING
    // Write the entity's cache index (i) into the pick output slot.
    // This is a uint reinterpreted as float in the SoA buffer; the vertex
    // shader reads it back as uint via asuint(). CPU resolves cache index
    // to EntityRecord on readback.
    outBuf[(SemEntityCacheIdx - 1u) * stride + outIdx] = asfloat(i);
#endif
}