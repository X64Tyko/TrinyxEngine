// build_draws.slang — Pass 4 of 5.
//
// Single workgroup, 256 threads (one per possible mesh type).
// Reads MeshHistogram (populated by scatter), computes an exclusive prefix
// sum to determine per-mesh base offsets in the sorted instance buffer,
// then writes one VkDrawIndexedIndirectCommand per mesh type.
//
// Also initializes MeshWriteIdx[meshID] = baseOffset for the sort pass,
// and clears MeshHistogram for the next frame.

#include "common/GpuFrameData.slang"

[[vk::push_constant]]
cbuffer PushConstants { uint64_t gFrameDataAddr; }

groupshared uint s_hist[256];
groupshared uint s_prefix[256];

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint tid : SV_GroupIndex)
{
    GpuFrameData* fd = (GpuFrameData*)gFrameDataAddr;
    uint meshCount   = fd->MeshCount;

    // Load histogram into shared memory
    uint* histogram = (uint*)fd->MeshHistogramAddr;
    uint count = (tid < meshCount) ? histogram[tid] : 0;
    s_hist[tid] = count;
    GroupMemoryBarrierWithGroupSync();

    // Exclusive prefix sum — sequential on thread 0 (256 iterations, trivial cost)
    if (tid == 0)
    {
        uint sum = 0;
        for (uint m = 0; m < 256; m++)
        {
            s_prefix[m] = sum;
            sum += s_hist[m];
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (tid < meshCount)
    {
        uint baseOffset = s_prefix[tid];

        // Read mesh geometry info from MeshTable
        GpuMeshInfo* meshTable = (GpuMeshInfo*)fd->MeshTableAddr;
        GpuMeshInfo info = meshTable[tid];

        // Write VkDrawIndexedIndirectCommand (5 uint32s per entry)
        // { indexCount, instanceCount, firstIndex, vertexOffset, firstInstance }
        uint* drawArgs = (uint*)fd->DrawArgsAddr;
        drawArgs[tid * 5 + 0] = info.IndexCount;
        drawArgs[tid * 5 + 1] = count;              // instanceCount from histogram
        drawArgs[tid * 5 + 2] = info.FirstIndex;
        drawArgs[tid * 5 + 3] = uint(info.VertexOffset); // int32 stored as uint32
        drawArgs[tid * 5 + 4] = baseOffset;          // firstInstance

        // Set base offset for sort pass (sort will atomicAdd from here)
        ((uint*)fd->MeshWriteIdxAddr)[tid] = baseOffset;

        // Clear histogram for next frame
        histogram[tid] = 0;
    }
}