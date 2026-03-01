// predicate.slang — Pass 1 of 3.
//
// Reads the active bit (bit 31) from each entity's flags and writes a raw
// 0/1 predicate value into the scan buffer.  The scan buffer doubles as
// temporary predicate storage; prefix_sum.slang overwrites it in-place with
// the final exclusive-scan output indices.

#include "GpuFrameData.slang"

[[vk::push_constant]]
cbuffer PushConstants { uint64_t gFrameDataAddr; }

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    GpuFrameData* fd = (GpuFrameData*)gFrameDataAddr;

    uint i = tid.x;
    if (i >= fd->EntityCount)
        return;

    uint flags = ((uint*)fd->FlagsAddr)[i];
    uint pred  = (flags >> 31u) & 1u;   // 1 if active, 0 otherwise

    // Write raw predicate into scan[i].  prefix_sum will overwrite in-place.
    ((uint*)fd->ScanAddr)[i] = pred;
}
