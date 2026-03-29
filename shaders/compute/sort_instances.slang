// sort_instances.slang — Pass 5 of 5.
//
// Reorders the compact unsorted instance buffer by MeshID so that each
// mesh type has a contiguous instance range in the sorted buffer.
//
// For each compact entity, reads MeshID from the unsorted buffer,
// atomically increments MeshWriteIdx[meshID] to get the destination index,
// and copies all SoA field slots to the sorted buffer.
//
// The sorted buffer is what the vertex shader reads from (InstancesAddr).
// Within a mesh type, ordering is undefined (atomic) — fine for rendering.

#include "common/GpuFrameData.slang"

[[vk::push_constant]]
cbuffer PushConstants { uint64_t gFrameDataAddr; }

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID)
{
    GpuFrameData* fd = (GpuFrameData*)gFrameDataAddr;

    // Read compact count — only process valid compact entities
    uint compactCount = ((uint*)fd->CompactCounterAddr)[0];
    uint srcIdx = gid.x;
    if (srcIdx >= compactCount)
        return;

    float* unsorted = (float*)fd->UnsortedInstancesAddr;
    float* sorted   = (float*)fd->InstancesAddr;
    uint   stride   = fd->OutFieldStride;

    // Read MeshID from unsorted buffer (slot = SemMeshID - 1 = 15)
    uint meshID = asuint(unsorted[(SemMeshID - 1u) * stride + srcIdx]);

    // Atomically claim a destination slot in the sorted buffer
    uint* writeIdx = (uint*)fd->MeshWriteIdxAddr;
    uint destIdx;
    InterlockedAdd(writeIdx[meshID], 1u, destIdx);

    // Copy all field slots from unsorted to sorted
    for (uint s = 0u; s < GpuOutFieldCount; s++)
    {
        sorted[s * stride + destIdx] = unsorted[s * stride + srcIdx];
    }

#ifdef TNX_GPU_PICKING
    sorted[(SemEntityCacheIdx - 1u) * stride + destIdx] =
        unsorted[(SemEntityCacheIdx - 1u) * stride + srcIdx];
#endif
}