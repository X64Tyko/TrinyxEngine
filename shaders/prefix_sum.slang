// prefix_sum.slang — Pass 2 of 3 (Option B prefix sum).
//
// Reads the raw 0/1 predicate written by predicate.slang into ScanAddr[i],
// computes an exclusive prefix sum using subgroup wave ops + one atomicAdd per
// workgroup, and writes the final per-entity scatter output index back to
// ScanAddr[i].
//
// Algorithm (64-thread workgroup):
//   1. Each lane: laneExcl  = WavePrefixCountBits(pred)  — count of active lanes below me
//                 waveTotal = WaveActiveCountBits(pred)   — total active in my wave
//   2. Wave lane-0 records waveTotal into groupshared s_waveSum[waveIdx].
//   3. Thread 0 sums all wave totals, atomicAdds to CompactCounterAddr to claim
//      a contiguous output range for this workgroup, stores base in s_wgBase.
//   4. Each thread: outIdx = s_wgBase + sum(s_waveSum[0..waveIdx-1]) + laneExcl
//   5. Write outIdx to ScanAddr[i] (only for active entities — scatter will
//      re-check the active flag, so writing for inactive lanes is harmless).
//
// This gives ~1563 atomicAdds for 100 k entities vs 100 k with per-entity atomic.

#include "GpuFrameData.slang"

[[vk::push_constant]]
cbuffer PushConstants { uint64_t gFrameDataAddr; }

// Up to 4 waves per workgroup (handles 16-lane subgroups on niche hardware;
// NVIDIA 32-lane = 2 waves, AMD 64-lane = 1 wave).
groupshared uint s_waveSum[4];
groupshared uint s_wgBase;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID,
          uint3 lid : SV_GroupThreadID)
{
    GpuFrameData* fd = (GpuFrameData*)gFrameDataAddr;

    uint i    = gid.x;
    uint pred = 0u;
    if (i < fd->EntityCount)
        pred = ((uint*)fd->ScanAddr)[i];   // raw 0/1 written by predicate pass

    bool bActive = (pred != 0u);

    // --- Step 1: wave-level exclusive scan ---
    uint laneExcl  = WavePrefixCountBits(bActive);  // exclusive count of active lanes before me
    uint waveTotal = WaveActiveCountBits(bActive);  // total active in this wave

    // Wave index within the 64-thread workgroup
    uint laneCount = WaveGetLaneCount();
    uint waveIdx   = lid.x / laneCount;

    // --- Step 2: wave lane-0 records this wave's total ---
    if (WaveIsFirstLane())
        s_waveSum[waveIdx] = waveTotal;

    GroupMemoryBarrierWithGroupSync();

    // --- Step 3: thread 0 atomicAdds the workgroup total, gets base ---
    if (lid.x == 0)
    {
        uint numWaves = (64u + laneCount - 1u) / laneCount;
        uint wgTotal  = 0u;
        for (uint w = 0u; w < numWaves; ++w)
            wgTotal += s_waveSum[w];

        uint prev;
        InterlockedAdd(((uint*)fd->CompactCounterAddr)[0], wgTotal, prev);
        s_wgBase = prev;
    }

    GroupMemoryBarrierWithGroupSync();

    // --- Step 4: compute per-wave offset and final output index ---
    uint waveOffset = 0u;
    for (uint w = 0u; w < waveIdx; ++w)
        waveOffset += s_waveSum[w];

    uint outIdx = s_wgBase + waveOffset + laneExcl;

    // --- Step 5: write back (scatter will re-check active flag) ---
    if (i < fd->EntityCount)
        ((uint*)fd->ScanAddr)[i] = outIdx;
}
